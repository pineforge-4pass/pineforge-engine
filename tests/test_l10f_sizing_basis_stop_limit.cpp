// R4-D L10f: on the switched (native) route, percent-of-equity entry sizing
// and stop-limit fill pricing reproduce legacy ab9714be reference behavior:
// 1. A limit entry sizes at fill-time basis using fill price.
// 2. A pure stop entry sizes at placement-time basis using level + slippage.
// 3. A stop-limit entry sizes at fill-time basis and fills at stop activation
//    (unslipped limit-or-better) or open-gap price.
#include "l4a_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"

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

#define CHECK(x) do {                                                           \
    if (x) {                                                                   \
        ++passed;                                                              \
    } else {                                                                   \
        ++failed;                                                              \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);               \
    }                                                                          \
} while (0)

bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}

Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

source::PineStrategyConfig make_cfg() {
    source::PineStrategyConfig c;
    c.initial_capital = 100000.0;
    c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
    c.default_qty_value = 5.0;
    c.commission_type = static_cast<int>(CommissionType::PERCENT);
    c.commission_value = 0.05;
    c.slippage = 1;
    c.margin_long = 100.0;
    c.margin_short = 100.0;
    c.pyramiding = 0;
    c.process_orders_on_close = false;
    c.calc_on_order_fills = false;
    return c;
}

class TestBaseHost : public source::PineStrategyHost {
public:
    TestBaseHost() {
        configure_pine_strategy(make_cfg());
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.00000001;
        syminfo_.pointvalue = 1.0;
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

// 1. Keystone limit replace: percent-of-equity limit entry sizes at fill-time basis
class KeystoneLimitHost : public TestBaseHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Keystone Pullback", true, 1800.17);
        } else if (pine_bar_index() == 2) {
            strategy_close("Keystone Pullback");
        }
    }
};

void test_keystone_limit_replay() {
    // 2025-03-31 13:15 signal bar, 13:30 fill bar, 13:45 close call, 14:00 exit fill
    std::vector<Bar> bars = {
        mk(1743426900000LL, 1824.79, 1829.13, 1821.36, 1822.92),
        mk(1743427800000LL, 1822.93, 1825.64, 1792.60, 1803.91),
        mk(1743428700000LL, 1803.90, 1823.60, 1797.77, 1817.66),
        mk(1743429600000LL, 1817.70, 1831.94, 1817.19, 1829.01),
    };
    KeystoneLimitHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.trade_count() == 1);
    // Legacy literals from base-ab9714b trade #1:
    // Qty: 2.77612739, Price: 1800.170000
    if (host.trade_count() >= 1) {
        const auto& trade = host.rows()[0];
        CHECK(near(trade.entry_price, 1800.170000));
        CHECK(near(trade.qty, 2.77612739, 1e-8));
        CHECK(trade.entry_bar_index == 1);
    }
}

// 2. Vector stop breakout: percent-of-equity pure stop entry sizes at placement basis with slippage
class VectorStopHost : public TestBaseHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Vector Breakout", true, kNaN, 1820.15);
        } else if (pine_bar_index() == 2) {
            strategy_close("Vector Breakout");
        }
    }
};

void test_vector_stop_replay() {
    // 2025-04-05 09:30 signal bar, 09:45 fill bar, 10:00 close call, 10:15 exit fill
    std::vector<Bar> bars = {
        mk(1743845400000LL, 1817.57, 1819.85, 1815.11, 1819.03),
        mk(1743846300000LL, 1819.04, 1821.26, 1818.53, 1819.87),
        mk(1743847200000LL, 1819.87, 1820.70, 1816.48, 1819.39),
        mk(1743848100000LL, 1819.40, 1819.60, 1815.51, 1817.16),
    };
    VectorStopHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.trade_count() == 1);
    // Legacy literals from base-ab9714b trade #1:
    // Qty: 2.74563843, Price: 1820.160000 (stop 1820.15 + slippage 1 tick = 1820.16)
    if (host.trade_count() >= 1) {
        const auto& trade = host.rows()[0];
        CHECK(near(trade.entry_price, 1820.160000));
        CHECK(near(trade.qty, 2.74563843, 1e-8));
        CHECK(trade.entry_bar_index == 1);
    }
}

// 3. Harbor stop limit: percent-of-equity stop-limit fills at stop activation price (unslipped)
class HarborStopLimitHost : public TestBaseHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Harbor Breakout", true, 1822.74, 1819.98);
        } else if (pine_bar_index() == 2) {
            strategy_close("Harbor Breakout");
        }
    }
};

void test_harbor_stop_limit_replay() {
    // 2025-03-31 12:15 signal bar, 12:30 fill bar, 12:45 close call, 13:00 exit fill
    std::vector<Bar> bars = {
        mk(1743423300000LL, 1809.49, 1818.80, 1808.15, 1816.41),
        mk(1743424200000LL, 1816.41, 1821.41, 1813.07, 1820.02),
        mk(1743425100000LL, 1820.02, 1845.78, 1817.89, 1833.49),
        mk(1743426000000LL, 1833.50, 1836.70, 1824.41, 1824.68),
    };
    HarborStopLimitHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.trade_count() == 1);
    // Legacy literals from base-ab9714b trade #1:
    // Qty: 2.74590998, Price: 1819.980000 (stop activates and limit is marketable: unslipped limit fill)
    if (host.trade_count() >= 1) {
        const auto& trade = host.rows()[0];
        CHECK(near(trade.entry_price, 1819.980000));
        CHECK(near(trade.qty, 2.74590998, 1e-8));
        CHECK(trade.entry_bar_index == 1);
    }
}

// 4. Harbor stop limit on open gap bar: fills at open price
class HarborStopLimitGapHost : public TestBaseHost {
public:
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) {
            strategy_entry("Harbor Breakout", true, 1822.74, 1819.98);
        } else if (pine_bar_index() == 2) {
            strategy_close("Harbor Breakout");
        }
    }
};

void test_harbor_stop_limit_gap_replay() {
    // Bar 1 open gapped above stop 1819.98, within limit 1822.74
    std::vector<Bar> bars = {
        mk(1743423300000LL, 1809.49, 1818.80, 1808.15, 1816.41),
        mk(1743424200000LL, 1821.00, 1825.00, 1820.00, 1823.00),
        mk(1743425100000LL, 1823.00, 1825.00, 1810.00, 1815.00),
        mk(1743426000000LL, 1815.00, 1820.00, 1810.00, 1812.00),
    };
    HarborStopLimitGapHost host;
    host.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(host.trade_count() == 1);
    if (host.trade_count() >= 1) {
        const auto& trade = host.rows()[0];
        // Sized at open price 1821.00: cash = 5000 / 1.0005 = 4997.501249375312
        // qty = 4997.501249375312 / 1821.00 = 2.74437191
        CHECK(near(trade.entry_price, 1821.000000));
        CHECK(near(trade.qty, 2.74437191, 1e-8));
        CHECK(trade.entry_bar_index == 1);
    }
}

} // namespace

int main() {
    test_keystone_limit_replay();
    test_vector_stop_replay();
    test_harbor_stop_limit_replay();
    test_harbor_stop_limit_gap_replay();
    std::printf("test_l10f_sizing_basis_stop_limit: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
