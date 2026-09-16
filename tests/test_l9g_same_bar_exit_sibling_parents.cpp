// R4-D L9g: a filled parent's same-bar priced exit is evaluated on the entry
// bar even when a sibling entry is pre-armed on the same bar (delta-3 P0-N4).
// Expectations are the legacy owner's (ab9714be) outputs for the same shapes.
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
int passed = 0;
int failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); } } while (0)

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(int pyr) {
    source::PineStrategyConfig c;
    c.initial_capital = 100000;
    c.default_qty_type = static_cast<int>(QtyType::FIXED);
    c.default_qty_value = 1.0;
    c.pyramiding = pyr;
    c.process_orders_on_close = false;
    return c;
}

class Host : public source::PineStrategyHost {
public:
    int variant = 0;
    Host(int v, int pyr) : variant(v) { configure_pine_strategy(cfg(pyr)); }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        // Opus delta-3 P0-N4 v1: market parent, exit declared before the
        // opposite breakout stop. Legacy: X fires at 3.0 on the entry bar.
        if (variant == 0) {
            strategy_entry("L", true);
            strategy_exit("X", "L", kNaN, 3.0);
            strategy_entry("S", false, kNaN, 2.5);
        }
        // Root probe4 v0: stop parent filled at the open with a sibling
        // same-direction stop entry; the exit still fires on the entry bar.
        if (variant == 1) {
            strategy_entry("L1", true, kNaN, 3.5);
            strategy_exit("X", "L1", kNaN, 3.0);
            strategy_entry("L2", true, kNaN, 5.0);
        }
    }
};

struct Row {
    const char* entry;
    const char* exit;
    double exit_price;
    std::int64_t exit_time;
};

void expect(const char* tag, int variant, int pyr, std::vector<Bar> bars,
            double pos, std::initializer_list<Row> rows) {
    Host host(variant, pyr);
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("%s\n", tag);
    CHECK(host.last_error().empty());
    CHECK(near(host.live_position_size(), pos));
    CHECK(host.trade_count() >= static_cast<int>(rows.size()));
    int i = 0;
    for (const auto& row : rows) {
        if (i >= host.trade_count()) break;
        const auto& t = host.get_trade(i++);
        CHECK(t.entry_id == row.entry);
        CHECK(t.exit_id == row.exit);
        CHECK(near(t.exit_price, row.exit_price));
        CHECK(t.exit_time == row.exit_time);
    }
}

} // namespace

int main() {
    // ab9714be: T0 L->X exit@3.0000 pos=-1 (S opens the short after the exit).
    expect("v0 market parent, X before S", 0, 1,
           {mk(1000, 4, 4, 4, 4), mk(2000, 4, 4.2, 2, 2.2), mk(3000, 2.2, 2.3, 2.1, 2.2), mk(4000, 2.2, 2.2, 2.2, 2.2)},
           -1.0, {{"L", "X", 3.0, 2000}});
    expect("v0 market parent, X before S (close 3.9)", 0, 1,
           {mk(1000, 4, 4, 4, 4), mk(2000, 4, 4.2, 2, 3.9), mk(3000, 2.2, 2.3, 2.1, 2.2), mk(4000, 2.2, 2.2, 2.2, 2.2)},
           -1.0, {{"L", "X", 3.0, 2000}});
    // ab9714be: L1@2000:4 -> X@2000:3.0 (the exit fires on the entry bar,
    // not one bar later). The sibling L2's own timing is pinned elsewhere.
    expect("v1 stop parent at the open, sibling stop entry", 1, 3,
           {mk(1000, 4, 4, 4, 4), mk(2000, 4, 5.5, 2, 4), mk(3000, 4, 4, 4, 4), mk(4000, 4, 4, 4, 4)},
           1.0, {{"L1", "X", 3.0, 2000}});
    std::printf("test_l9g_same_bar_exit_sibling_parents: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
