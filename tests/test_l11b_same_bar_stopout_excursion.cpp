// R4-D L11b: same-bar stopout excursion parity.
// Pins the owner rows of arun-rajan #1-#3 and geckin-joey #1-#2 with embedded
// bars, and verifies host-owned excursion accounting for same-bar stopouts:
// (a) [retired with the host model, R5 lane H-THIN E19];
// (b) a 100%-of-equity long entry opening slice preceding a priced exit
//     preserves the exit-fill loss in adverse excursion (yukozb).
// Bars are embedded literals from corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv.
#include "l4a_native_route_guard.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
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

bool near(double a, double b, double tol = 1e-4) {
    return std::abs(a - b) <= tol;
}

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig arun_cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 0;
    c.process_orders_on_close = true;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

source::PineStrategyConfig joey_cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000.0;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 0;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

void expect_trade(const char* tag, const Trade& t, bool is_long,
                  double entry_px, double exit_px, double pnl,
                  double fav, double adv) {
    std::printf("%s %s @%.4f->%.4f pnl=%.4f mfe=%.4f mae=%.4f (want @%.4f->%.4f pnl=%.4f mfe=%.4f mae=%.4f)\n",
                tag, is_long ? "L" : "S", t.entry_price, t.exit_price, t.pnl,
                t.max_runup, t.max_drawdown, entry_px, exit_px, pnl, fav, adv);
    CHECK(t.is_long == is_long);
    CHECK(near(t.entry_price, entry_px));
    CHECK(near(t.exit_price, exit_px));
    CHECK(near(t.pnl, pnl));
    CHECK(near(t.max_runup, fav));
    CHECK(near(t.max_drawdown, adv));
}

// Arun-Rajan Trade #1:
// Entry long 2025-04-02 09:00 close @ 1883.98, stopped out 2025-04-02 12:00 @ 1863.67.
// Owner literals: entry 1883.98, exit 1863.67, pnl -20.31, fav 2.72, adv 20.31.
class ArunRajanTrade1 : public source::PineStrategyHost {
public:
    ArunRajanTrade1() { configure_pine_strategy(arun_cfg()); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Buy", true);
            strategy_exit("Buy Exit", "Buy", kNaN, 1863.67);
        }
    }
};

// Arun-Rajan Trade #2:
// Entry long 2025-04-02 14:45 close @ 1874.69, take-profit 2025-04-02 15:30 @ 1897.90.
// Owner literals: entry 1874.69, exit 1897.90, pnl 23.21, fav 23.21, adv 1.53.
class ArunRajanTrade2 : public source::PineStrategyHost {
public:
    ArunRajanTrade2() { configure_pine_strategy(arun_cfg()); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Buy", true);
            strategy_exit("Buy Exit", "Buy", 1897.90, kNaN);
        }
    }
};

// Arun-Rajan Trade #3:
// Entry short 2025-04-03 09:45 close @ 1810.49, take-profit 2025-04-03 12:30 @ 1760.32.
// Owner literals: entry 1810.49, exit 1760.32, pnl 50.17, fav 50.17, adv 6.25.
class ArunRajanTrade3 : public source::PineStrategyHost {
public:
    ArunRajanTrade3() { configure_pine_strategy(arun_cfg()); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Sell", false);
            strategy_exit("Sell Exit", "Sell", 1760.32, kNaN);
        }
    }
};

// Geckin-Joey Trade #1:
// Signal 2025-03-31 21:30, filled 21:45 @ 1824.15 open, TP 2025-04-01 00:45 @ 1837.23.
// Owner literals: entry 1824.15, exit 1837.23, pnl 13.08, fav 13.08, adv 8.93.
class JoeyTrade1 : public source::PineStrategyHost {
public:
    JoeyTrade1() { configure_pine_strategy(joey_cfg()); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Enter", true);
        } else if (pine_bar_index() == 1) {
            strategy_exit("Exit", "Enter", 1837.23, 1811.07);
        }
    }
};

// Geckin-Joey Trade #2:
// Signal 2025-04-01 19:45, filled 20:00 @ 1909.11 open, SL 2025-04-02 00:30 @ 1895.63.
// Owner literals: entry 1909.11, exit 1895.63, pnl -13.48, fav 12.87, adv 13.48.
class JoeyTrade2 : public source::PineStrategyHost {
public:
    JoeyTrade2() { configure_pine_strategy(joey_cfg()); set_syminfo_metadata("ETHUSDT", 0.01); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Enter", true);
        } else if (pine_bar_index() == 1) {
            strategy_exit("Exit", "Enter", 1922.59, 1895.63);
        }
    }
};

// Yukozb trade #53 same-bar stopout shape:
// 100%-of-equity long entry admitted and split into main lot + opening slice,
// followed on the same bar by stop-out at 3111.28.
// Owner literals for trade #53:
//   entry 3130.30, exit 3130.26, qty 0.00004978, pnl -0.000064, mfe 0.000000, mae -0.000033.
source::PineStrategyConfig yukozb_cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 10000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 100.0;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = 0.02;
    c.slippage = 2;
    c.process_orders_on_close = false;
    c.pyramiding = 1;
    return c;
}

class YukozbSameBarStopout : public source::PineStrategyHost {
public:
    YukozbSameBarStopout() {
        configure_pine_strategy(yukozb_cfg());
        set_syminfo_metadata("ETHUSDT", 0.01);
        set_syminfo_metadata("qty_step", 0.00000001);
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("Long", true);
            strategy_exit("LongX", "Long", 1830.00, 1801.00, kNaN, kNaN, kNaN, 100.0);
        }
    }
};

}  // namespace

int main() {
    // -----------------------------------------------------------------------
    // Arun-Rajan Trade #1
    // -----------------------------------------------------------------------
    {
        ArunRajanTrade1 host;
        const std::vector<Bar> bars = {
            mk(1743584400000LL, 1866.90, 1887.23, 1865.38, 1883.98),  // 09:00
            mk(1743585300000LL, 1883.98, 1885.00, 1874.00, 1878.59),
            mk(1743586200000LL, 1878.58, 1881.73, 1876.00, 1876.58),
            mk(1743587100000LL, 1876.57, 1880.38, 1874.98, 1878.44),
            mk(1743588000000LL, 1878.43, 1883.80, 1876.15, 1880.03),
            mk(1743588900000LL, 1880.02, 1886.70, 1878.19, 1878.91),  // 10:15 (H=1886.70 => fav=2.72)
            mk(1743589800000LL, 1878.90, 1878.98, 1871.85, 1874.48),
            mk(1743590700000LL, 1874.47, 1876.52, 1870.00, 1871.92),
            mk(1743591600000LL, 1871.93, 1877.62, 1871.00, 1874.59),
            mk(1743592500000LL, 1874.59, 1877.53, 1872.50, 1872.60),
            mk(1743593400000LL, 1872.60, 1873.99, 1868.50, 1871.40),
            mk(1743594300000LL, 1871.39, 1872.64, 1867.32, 1868.05),
            mk(1743595200000LL, 1868.08, 1869.97, 1859.53, 1865.27),  // 12:00 (L=1859.53 => stop fill @ 1863.67)
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("arun-rajan#1", host.get_trade(0), true, 1883.98, 1863.67, -20.31, 2.72, 20.31);
        }
    }

    // -----------------------------------------------------------------------
    // Arun-Rajan Trade #2
    // -----------------------------------------------------------------------
    {
        ArunRajanTrade2 host;
        const std::vector<Bar> bars = {
            mk(1743605100000LL, 1870.74, 1877.04, 1868.55, 1874.69),  // 14:45
            mk(1743606000000LL, 1874.70, 1894.47, 1873.16, 1881.00),  // 15:00 (L=1873.16 => adv=1.53)
            mk(1743606900000LL, 1881.00, 1893.06, 1880.50, 1889.52),
            mk(1743607800000LL, 1889.52, 1918.00, 1889.30, 1913.94),  // 15:30 (H=1918.00 => limit fill @ 1897.90)
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("arun-rajan#2", host.get_trade(0), true, 1874.69, 1897.90, 23.21, 23.21, 1.53);
        }
    }

    // -----------------------------------------------------------------------
    // Arun-Rajan Trade #3
    // -----------------------------------------------------------------------
    {
        ArunRajanTrade3 host;
        const std::vector<Bar> bars = {
            mk(1743673500000LL, 1819.70, 1819.71, 1808.27, 1810.49),  // 09:45
            mk(1743674400000LL, 1810.48, 1816.74, 1808.24, 1811.57),  // 10:00 (H=1816.74 => adv=6.25)
            mk(1743675300000LL, 1811.58, 1813.40, 1797.15, 1799.49),
            mk(1743676200000LL, 1799.40, 1802.86, 1789.09, 1798.19),
            mk(1743677100000LL, 1798.17, 1802.09, 1790.01, 1792.39),
            mk(1743678000000LL, 1792.39, 1801.81, 1786.14, 1800.91),
            mk(1743678900000LL, 1800.91, 1801.68, 1792.06, 1795.68),
            mk(1743679800000LL, 1795.69, 1799.10, 1791.40, 1797.93),
            mk(1743680700000LL, 1797.93, 1798.60, 1792.12, 1794.39),
            mk(1743681600000LL, 1794.39, 1799.82, 1789.77, 1797.79),
            mk(1743682500000LL, 1797.78, 1797.78, 1785.03, 1787.76),
            mk(1743683400000LL, 1787.77, 1794.92, 1759.81, 1766.99),  // 12:30 (L=1759.81 => limit fill @ 1760.32)
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("arun-rajan#3", host.get_trade(0), false, 1810.49, 1760.32, 50.17, 50.17, 6.25);
        }
    }

    // -----------------------------------------------------------------------
    // Geckin-Joey Trade #1
    // -----------------------------------------------------------------------
    {
        JoeyTrade1 host;
        const std::vector<Bar> bars = {
            mk(1743456600000LL, 1825.01, 1825.81, 1823.31, 1824.15),  // 21:30 signal
            mk(1743457500000LL, 1824.15, 1826.50, 1820.79, 1823.61),  // 21:45 entry @ 1824.15
            mk(1743458400000LL, 1823.60, 1823.60, 1816.75, 1818.27),
            mk(1743459300000LL, 1818.27, 1821.00, 1815.22, 1820.32),  // 22:15 (L=1815.22 => adv=8.93)
            mk(1743460200000LL, 1820.32, 1823.68, 1820.07, 1821.91),
            mk(1743461100000LL, 1821.91, 1824.46, 1820.86, 1823.96),
            mk(1743462000000LL, 1823.95, 1827.51, 1822.71, 1825.08),
            mk(1743462900000LL, 1825.09, 1831.00, 1819.17, 1822.86),
            mk(1743463800000LL, 1822.86, 1825.97, 1820.00, 1823.78),
            mk(1743464700000LL, 1823.77, 1823.87, 1820.00, 1821.59),
            mk(1743465600000LL, 1821.59, 1826.24, 1819.01, 1821.47),
            mk(1743466500000LL, 1821.48, 1829.36, 1820.11, 1826.38),
            mk(1743467400000LL, 1826.37, 1828.80, 1822.33, 1824.92),
            mk(1743468300000LL, 1824.93, 1842.57, 1816.64, 1831.20),  // 00:45 (H=1842.57 => TP fill @ 1837.23)
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("geckin-joey#1", host.get_trade(0), true, 1824.15, 1837.23, 13.08, 13.08, 8.93);
        }
    }

    // -----------------------------------------------------------------------
    // Geckin-Joey Trade #2
    // -----------------------------------------------------------------------
    {
        JoeyTrade2 host;
        const std::vector<Bar> bars = {
            mk(1743536700000LL, 1905.99, 1910.50, 1904.70, 1909.12),  // 19:45 signal
            mk(1743537600000LL, 1909.11, 1909.75, 1904.16, 1908.36),  // 20:00 entry @ 1909.11
            mk(1743538500000LL, 1908.35, 1912.65, 1900.00, 1901.60),
            mk(1743539400000LL, 1901.61, 1907.49, 1895.90, 1905.00),
            mk(1743540300000LL, 1905.00, 1915.20, 1904.60, 1912.06),
            mk(1743541200000LL, 1912.07, 1916.56, 1907.55, 1913.53),
            mk(1743542100000LL, 1913.52, 1916.11, 1912.43, 1914.01),
            mk(1743543000000LL, 1914.00, 1921.98, 1910.70, 1917.24),  // 21:30 (H=1921.98 => fav=12.87)
            mk(1743543900000LL, 1917.23, 1917.24, 1911.61, 1914.19),
            mk(1743544800000LL, 1914.20, 1916.73, 1910.76, 1915.54),
            mk(1743545700000LL, 1915.55, 1917.10, 1912.69, 1916.01),
            mk(1743546600000LL, 1916.02, 1919.30, 1914.21, 1914.47),
            mk(1743547500000LL, 1914.47, 1916.00, 1910.09, 1911.00),
            mk(1743548400000LL, 1911.01, 1912.13, 1903.86, 1904.52),
            mk(1743549300000LL, 1904.52, 1910.64, 1904.51, 1910.06),
            mk(1743550200000LL, 1910.05, 1914.36, 1908.00, 1909.32),
            mk(1743551100000LL, 1909.32, 1909.32, 1903.77, 1904.21),
            mk(1743552000000LL, 1904.21, 1908.01, 1900.09, 1900.21),
            mk(1743552900000LL, 1900.21, 1904.46, 1896.20, 1896.99),
            mk(1743553800000LL, 1896.99, 1899.98, 1891.62, 1893.74),  // 00:30 (L=1891.62 => SL fill @ 1895.63)
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 1);
        if (host.trade_count() >= 1) {
            expect_trade("geckin-joey#2", host.get_trade(0), true, 1909.11, 1895.63, -13.48, 12.87, 13.48);
        }
    }

    // The former "same-bar short stopout: low-first bar with low masked"
    // section (zz-pop-ahtisham trade #83 / zz-pop-waranyutrkm trade #66)
    // reached the host's own excursion model directly with owner facts. R5
    // lane H-THIN (E19) retired that model -- the kernel samples the path it
    // walks -- so the section went with it.

    // -----------------------------------------------------------------------
    // Same-bar long margin call slice with fill loss
    // (zz-pop-yukozb trade #53 shape)
    // -----------------------------------------------------------------------
    {
        YukozbSameBarStopout host;
        const std::vector<Bar> bars = {
            mk(1743778800000LL, 1789.47, 1814.99, 1788.34, 1812.32),  // 0: 15:00 signal
            mk(1743779700000LL, 1812.33, 1820.18, 1800.63, 1802.28),  // 1: 15:15 entry @ 1812.35, SL @ 1801.00
            mk(1743780600000LL, 1802.27, 1806.77, 1786.40, 1791.59),  // 2: 15:30
        };
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.last_error().empty());
        CHECK(host.trade_count() == 2);
        if (host.trade_count() >= 2) {
            // Trade #0: opening margin call slice
            const Trade& slice = host.get_trade(0);
            CHECK(slice.is_long);
            CHECK(near(slice.entry_price, 1812.35));
            CHECK(near(slice.exit_price, 1812.31));
            CHECK(near(slice.max_runup, 0.0));
            // Adverse excursion includes exit-fill loss (0.04 * qty) + entry commission:
            // 0.04 * 0.00012172 + 0.0002 * 1812.35 * 0.00012172 = 0.00004899
            CHECK(near(slice.max_drawdown, 0.00004899, 1e-6));
        }
    }

    std::printf("test_l11b_same_bar_stopout_excursion: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
