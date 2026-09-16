// R4-D L9b P0-B: opposite marketable stop entries at the open follow the
// legacy fill order (ab9714be pine_fills.cpp:3687-3860 fill_phase +
// opposing_stop deferral). Literals are the ab9714be output of
// EV/tasks/r4-d/fable-delta3-probes/ord/probe.cpp (base.txt).
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

Bar mk(std::int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return {o, h, l, c, v, t};
}

source::PineStrategyConfig cfg(double cap, int qtype, double qval, int pyr, bool pooc) {
    source::PineStrategyConfig c;
    c.initial_capital = cap;
    c.default_qty_type = qtype;
    c.default_qty_value = qval;
    c.pyramiding = pyr;
    c.process_orders_on_close = pooc;
    return c;
}

class OrderHost : public source::PineStrategyHost {
public:
    int variant = 0;
    explicit OrderHost(int v) : variant(v) {
        configure_pine_strategy(cfg(100000, (int)QtyType::FIXED, 1.0, 3, false));
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        if (variant == 0) {
            strategy_entry("A", true, kNaN, 5.0);
            strategy_entry("B", false, kNaN, 5.0);
            strategy_entry("C", true, kNaN, 3.0);
        }
        if (variant == 1) {
            strategy_entry("C", true, kNaN, 3.0);
            strategy_entry("B", false, kNaN, 5.0);
            strategy_entry("A", true, kNaN, 5.0);
        }
        if (variant == 2) {
            strategy_entry("B", false, kNaN, 3.0);
            strategy_entry("A", true, kNaN, 5.0);
            strategy_entry("D", true, kNaN, kNaN);
            strategy_entry("C", true, kNaN, 4.5);
        }
        if (variant == 3) {
            strategy_entry("A", true, kNaN, 5.0);
            strategy_entry("B", false, kNaN, 5.0);
            strategy_entry("C", true, kNaN, 3.0);
            strategy_entry("D", false, kNaN, 2.5);
        }
    }
};

struct TradeLiteral {
    const char* entry;
    const char* exit;
    double qty;
    double exit_price;
};

void expect(const char* tag, int variant, bool high_first,
            int trades, double pos, std::initializer_list<TradeLiteral> rows) {
    OrderHost host(variant);
    std::vector<Bar> bars;
    if (high_first) {
        bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2, 4), mk(3000, 4, 4, 4, 4)};
    } else {
        bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2, 5), mk(3000, 4, 4, 4, 4)};
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("%s\n", tag);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == trades);
    CHECK(near(host.live_position_size(), pos));
    CHECK(static_cast<int>(rows.size()) == trades);
    int i = 0;
    for (const auto& row : rows) {
        if (i >= host.trade_count()) break;
        const auto& t = host.get_trade(i++);
        CHECK(t.entry_id == row.entry);
        CHECK(t.exit_id == row.exit);
        CHECK(near(t.qty, row.qty));
        CHECK(near(t.exit_price, row.exit_price));
        CHECK(t.exit_comment.empty());
    }
}

} // namespace

int main() {
    expect("ord v0 high-first", 0, true, 1, 1.0, {{"C", "B", 1.0, 4.0}});
    expect("ord v0 low-first", 0, false, 1, 1.0, {{"C", "B", 1.0, 4.0}});
    expect("ord v1 high-first", 1, true, 1, 0.0, {{"C", "B", 1.0, 4.0}});
    expect("ord v1 low-first", 1, false, 1, 0.0, {{"C", "B", 1.0, 4.0}});
    expect("ord v2 high-first", 2, true, 1, 0.0, {{"D", "B", 1.0, 3.0}});
    expect("ord v2 low-first", 2, false, 1, 0.0, {{"D", "B", 1.0, 3.0}});
    expect("ord v3 high-first", 3, true, 2, 0.0,
           {{"C", "D", 1.0, 2.5}, {"A", "B", 1.0, 4.0}});
    expect("ord v3 low-first", 3, false, 2, 0.0,
           {{"C", "D", 1.0, 2.5}, {"A", "B", 1.0, 4.0}});
    std::printf("test_l9b_open_marketable_order: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
