// R4-D L10e: a same-pass close then opposite stop fills the short at the
// stop level (ab9714be pine_fills.cpp:8013-8018), not at the close's open.
// Literals are trade #3 of order-opposite-entry-close-same-pass-01 and
// pyramid-flip-stop-pyramiding-2-01 (engine ab9714b) on the 15m ETH feed.
#include "l4a_native_route_guard.hpp"

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

bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

source::PineStrategyConfig cfg(int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = 1000000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyr;
    c.process_orders_on_close = false;
    c.commission_value = 0.0;
    c.slippage = 0;
    return c;
}

class Host : public source::PineStrategyHost {
public:
    explicit Host(int pyr) {
        configure_pine_strategy(cfg(pyr));
        set_syminfo_metadata("ETHUSDT", 0.01);
    }
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        if (i == 0 && live_position_size() == 0.0) {
            strategy_entry("L2", true, kNaN, kNaN, 1.0, "open long2");
        }
        if (i == 2 && live_position_size() > 0.0) {
            strategy_close("L2", "close long first");
            strategy_entry("S2", false, kNaN, bar.low - 0.01, 1.0,
                           "opposite stop second");
        }
        if (i == 4 && live_position_size() < 0.0) strategy_close_all();
    }
};

void expect(const char* tag, int pyr) {
    Host host(pyr);
    const std::vector<Bar> bars = {
        mk(1743401700000, 1804.12, 1807.92, 1800.49, 1801.3),
        mk(1743402600000, 1801.3, 1804.99, 1798.42, 1804.37),
        mk(1743403500000, 1804.36, 1806.49, 1800.01, 1803.22),
        mk(1743404400000, 1803.22, 1809.85, 1798.12, 1800.54),
        mk(1743423300000, 1809.49, 1818.8, 1808.15, 1816.41),
        mk(1743424200000, 1816.41, 1821.41, 1813.07, 1820.02),
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("%s\n", tag);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    CHECK(near(host.live_position_size(), 0.0));
    if (host.trade_count() < 2) return;
    const auto& long_close = host.get_trade(0);
    CHECK(long_close.entry_id == "L2");
    CHECK(long_close.entry_time == 1743402600000);
    CHECK(near(long_close.entry_price, 1801.3));
    CHECK(long_close.exit_time == 1743404400000);
    CHECK(near(long_close.exit_price, 1803.22));
    CHECK(near(long_close.qty, 1.0));
    CHECK(long_close.is_long);
    const auto& short_entry = host.get_trade(1);
    CHECK(short_entry.entry_id == "S2");
    CHECK(short_entry.entry_time == 1743404400000);
    CHECK(near(short_entry.entry_price, 1800.0));
    CHECK(short_entry.exit_time == 1743424200000);
    CHECK(near(short_entry.exit_price, 1816.41));
    CHECK(near(short_entry.qty, 1.0));
    CHECK(!short_entry.is_long);
    CHECK(near(short_entry.pnl, -16.41));
}

}  // namespace

int main() {
    expect("order-opposite-entry-close-same-pass-01 trade #3", 1);
    expect("pyramid-flip-stop-pyramiding-2-01 trade #3", 2);
    std::printf("test_l10e_opposite_entry_close_same_pass: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
