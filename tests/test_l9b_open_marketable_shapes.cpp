// R4-D L9b P0-B: two opposite stop entries, including both-marketable-at-open
// and the |h-o|=|o-l| path tie. Literals are the ab9714be output of
// EV/tasks/r4-d/fable-delta3-probes/ord/probe2.cpp (base2.txt).
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

class ShapeHost : public source::PineStrategyHost {
public:
    int variant = 0;
    explicit ShapeHost(int v) : variant(v) {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = 1;
        c.process_orders_on_close = false;
        configure_pine_strategy(c);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        if (variant == 0) {
            strategy_entry("Long", true, kNaN, 5.0);
            strategy_entry("Short", false, kNaN, 3.0);
        }
        if (variant == 1) {
            strategy_entry("Short", false, kNaN, 3.0);
            strategy_entry("Long", true, kNaN, 5.0);
        }
        if (variant == 2) {
            strategy_entry("Long", true, kNaN, 4.5);
            strategy_entry("Short", false, kNaN, 4.5);
        }
        if (variant == 3) {
            strategy_entry("Short", false, kNaN, 4.5);
            strategy_entry("Long", true, kNaN, 4.5);
        }
        if (variant == 4) {
            strategy_entry("Long", true, kNaN, 3.5);
            strategy_entry("Short", false, kNaN, 4.5);
        }
        if (variant == 5) {
            strategy_entry("Short", false, kNaN, 4.5);
            strategy_entry("Long", true, kNaN, 3.5);
        }
    }
};

struct Expectation {
    const char* entry;
    const char* exit;
    double exit_price;
};

void expect(int variant, int shape, Expectation row) {
    ShapeHost host(variant);
    std::vector<Bar> bars;
    if (shape == 0) bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2, 4), mk(3000, 4, 4, 4, 4)};
    if (shape == 1) bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2.5, 4), mk(3000, 4, 4, 4, 4)};
    if (shape == 2) bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 5.5, 2, 4), mk(3000, 4, 4, 4, 4)};
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("v%d shape%d\n", variant, shape);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(near(host.live_position_size(), 0.0));
    if (host.trade_count() < 1) return;
    const auto& t = host.get_trade(0);
    CHECK(t.entry_id == row.entry);
    CHECK(t.exit_id == row.exit);
    CHECK(near(t.qty, 1.0));
    CHECK(near(t.exit_price, row.exit_price));
    CHECK(t.exit_comment.empty());
}

class PoocShapeHost : public source::PineStrategyHost {
public:
    int variant = 0;
    explicit PoocShapeHost(int v, int pyr) : variant(v) {
        source::PineStrategyConfig c;
        c.initial_capital = 100000;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 1.0;
        c.pyramiding = pyr;
        c.process_orders_on_close = true;
        configure_pine_strategy(c);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        if (variant == 0) {
            strategy_entry("S1", false, kNaN, 4.5);
            strategy_entry("A", true, kNaN, 3.5);
            strategy_entry("S2", false, kNaN, 1.0);
        }
        if (variant == 1) {
            strategy_entry("Short", false, kNaN, 4.5);
            strategy_entry("Long", true, kNaN, 3.5);
        }
        if (variant == 2) {
            strategy_entry("S1", false, kNaN, 4.5);
            strategy_entry("A", true, kNaN, 3.5);
            strategy_entry("S2", false, kNaN, 2.5);
        }
    }
};

void expect_pooc(const char* tag, int variant, int pyr, double pos,
                 const char* entry, const char* exit, double exit_price) {
    PoocShapeHost host(variant, pyr);
    // ab9714be l9b probe.cpp bars (P0-N3 POOC rows)
    std::vector<Bar> bars = {
        mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2, 4), mk(3000, 4, 4.2, 3.8, 4),
        mk(4000, 4, 4.2, 0.5, 4), mk(5000, 4, 4, 4, 4)};
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("%s\n", tag);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    CHECK(near(host.live_position_size(), pos));
    if (host.trade_count() < 1) return;
    const auto& t = host.get_trade(0);
    CHECK(t.entry_id == entry);
    CHECK(t.exit_id == exit);
    CHECK(near(t.qty, 1.0));
    CHECK(near(t.exit_price, exit_price));
    CHECK(t.exit_comment.empty());
}

} // namespace

int main() {
    // ab9714be probe2_base / base2.txt
    expect(0, 0, {"Short", "Long", 5.0});
    expect(0, 1, {"Short", "Long", 5.0});
    expect(0, 2, {"Long", "Short", 3.0});
    expect(1, 0, {"Short", "Long", 5.0});
    expect(1, 1, {"Short", "Long", 5.0});
    expect(1, 2, {"Long", "Short", 3.0});
    expect(2, 0, {"Short", "Long", 4.5});
    expect(2, 1, {"Short", "Long", 4.5});
    expect(2, 2, {"Short", "Long", 4.5});
    expect(3, 0, {"Short", "Long", 4.5});
    expect(3, 1, {"Short", "Long", 4.5});
    expect(3, 2, {"Short", "Long", 4.5});
    expect(4, 0, {"Long", "Short", 4.0});
    expect(4, 1, {"Long", "Short", 4.0});
    expect(4, 2, {"Long", "Short", 4.0});
    expect(5, 0, {"Long", "Short", 4.0});
    expect(5, 1, {"Long", "Short", 4.0});
    expect(5, 2, {"Long", "Short", 4.0});
    // ab9714be l9b/base.txt POOC rows (P0-N3)
    expect_pooc("S1,A,S2@1(untouched) pyr1 pooc", 0, 1, -1.0, "A", "S1", 4.0);
    expect_pooc("S1,A,S2@1(untouched) pyr3 pooc", 0, 3, -1.0, "A", "S1", 4.0);
    expect_pooc("P0-B v5 pyr1 pooc", 1, 1, 0.0, "Long", "Short", 4.0);
    expect_pooc("P0-B v5 pyr3 pooc", 1, 3, 0.0, "Long", "Short", 4.0);
    expect_pooc("S1,A,S2@2.5(touched) pyr1 pooc", 2, 1, -1.0, "A", "S2", 2.5);
    expect_pooc("S1,A,S2@2.5(touched) pyr3 pooc", 2, 3, -1.0, "A", "S2", 2.5);
    std::printf("test_l9b_open_marketable_shapes: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
