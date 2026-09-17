// R4-D L10n: a strategy.exit re-issued with the SAME exit id for a new
// entry cycle (after the previous cycle flattened, including by
// strategy.close_all) attaches to the new entry and fills, matching
// ab9714be. Probe9 "A v0" owner literals are pinned here with embedded bars.
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
    c.initial_capital = 100000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = 1;
    c.process_orders_on_close = false;
    c.margin_long = 100;
    c.margin_short = 100;
    return c;
}

class ProbeA : public source::PineStrategyHost {
public:
    ProbeA() { configure_pine_strategy(cfg()); }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 1) {
            strategy_entry("Long", true, kNaN, kNaN, 1.0);
            strategy_exit("TP/SL 1", "Long", 1830.0, 1780.0, kNaN, kNaN, kNaN,
                          100.0, "", 1.0);
        }
        if (i == 8) strategy_close_all();
        if (i == 9) {
            strategy_entry("Long", true, kNaN, kNaN, 1.0);
            strategy_exit("TP/SL 1", "Long", 1830.0, 1780.0, kNaN, kNaN, kNaN,
                          100.0, "", 1.0);
        }
    }
};

std::vector<Bar> probe_bars() {
    std::vector<Bar> b;
    for (int i = 0; i < 12; ++i) {
        const double base = 1800 + (i % 3) * 5;
        b.push_back(mk(1000 * (i + 1), base, base + 40, base - 30, base + 10));
    }
    b[2] = mk(3000, 1805, 1835, 1790, 1810);
    b[3] = mk(4000, 1810, 1845, 1800, 1840);
    return b;
}

}  // namespace

int main() {
    ProbeA host;
    const auto bars = probe_bars();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 2);
    CHECK(near(host.live_position_size(), 0.0));
    CHECK(host.trade_count() >= 2);
    if (host.trade_count() >= 2) {
        const auto& t0 = host.get_trade(0);
        const auto& t1 = host.get_trade(1);
        CHECK(t0.entry_id == "Long");
        CHECK(t0.exit_id == "TP/SL 1");
        CHECK(t0.entry_time == 3000);
        CHECK(near(t0.entry_price, 1805.0));
        CHECK(t0.exit_time == 3000);
        CHECK(near(t0.exit_price, 1830.0));
        CHECK(near(t0.qty, 1.0));
        CHECK(t1.entry_id == "Long");
        CHECK(t1.exit_id == "TP/SL 1");
        CHECK(t1.entry_time == 11000);
        CHECK(near(t1.entry_price, 1805.0));
        CHECK(t1.exit_time == 11000);
        CHECK(near(t1.exit_price, 1780.0));
        CHECK(near(t1.qty, 1.0));
    }
    std::printf("test_l10n_exit_reissue_after_close_all: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
