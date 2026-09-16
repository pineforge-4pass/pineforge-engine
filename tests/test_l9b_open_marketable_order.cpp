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
        if (pine_bar_index() == 3) { strategy_close_all(); return; }
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
        // L9d (legacy ab9714be pinned): S2 rests below the bar's low.
        if (variant == 4) {
            strategy_entry("S1", false, kNaN, 4.5);
            strategy_entry("L", true, kNaN, 3.5);
            strategy_entry("S2", false, kNaN, 1.0);
        }
        // L9d: S2 closes L on the path; S1 may not reopen from flat this bar.
        if (variant == 5) {
            strategy_entry("S1", false, kNaN, 4.5);
            strategy_entry("L", true, kNaN, 3.5);
            strategy_entry("S2", false, kNaN, 2.5);
        }
        // L9d: resting S2 closes the pyramided L2 on the later bar.
        if (variant == 6) {
            strategy_entry("S1", false, kNaN, 4.5);
            strategy_entry("L", true, kNaN, 3.5);
            strategy_entry("S2", false, kNaN, 1.0);
            strategy_entry("L2", true, kNaN, 5.5);
        }
    }
};

struct TradeLiteral {
    const char* entry;
    const char* exit;
    double qty;
    double exit_price;
    std::int64_t entry_time = 0;   // 0 = not pinned
    double entry_price = 0.0;      // pinned only with entry_time
};

void expect(const char* tag, int variant, bool high_first,
            int trades, double pos, std::initializer_list<TradeLiteral> rows,
            int later_shape = 0) {
    OrderHost host(variant);
    std::vector<Bar> bars;
    if (high_first) {
        bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2, 4), mk(3000, 4, 4, 4, 4)};
    } else {
        bars = {mk(1000, 4, 4, 4, 4), mk(2000, 4, 6, 2, 5), mk(3000, 4, 4, 4, 4)};
    }
    // later_shape 1: the bar after the deferral bar reaches 0.5, so a resting
    // stop at 1.0 fills there and not on the deferral bar.
    if (later_shape == 1) bars[2] = mk(3000, 4, 4, 0.5, 4);
    // later_shape 2: a fourth bar on which the host closes everything (the
    // market close fills at the fifth bar's open), so an open position's
    // entry bar and price become a pinned trade row.
    if (later_shape == 2) {
        bars.push_back(mk(4000, 4, 4, 4, 4));
        bars.push_back(mk(5000, 4, 4, 4, 4));
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
        if (row.entry_time != 0) {
            CHECK(t.entry_time == row.entry_time);
            CHECK(near(t.entry_price, row.entry_price));
        }
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
    // L9d: an untouched resting stop never fills on the deferral bar.
    expect("ord v4 high-first (S2 rests)", 4, true, 1, 0.0,
           {{"L", "S1", 1.0, 4.0, 2000, 4.0}});
    expect("ord v4 low-first (S2 rests)", 4, false, 1, 0.0,
           {{"L", "S1", 1.0, 4.0, 2000, 4.0}});
    expect("ord v4 later low 0.5 (S2 fills next bar)", 4, true, 1, -1.0,
           {{"L", "S1", 1.0, 4.0, 2000, 4.0}}, 1);
    // L9d: S1 is throttled to the next bar's open after S2 closed L.
    expect("ord v5 high-first (S1 next bar)", 5, true, 2, 0.0,
           {{"L", "S2", 1.0, 2.5, 2000, 4.0}, {"S1", "__close__", 1.0, 4.0, 3000, 4.0}}, 2);
    expect("ord v5 low-first (S1 next bar)", 5, false, 2, 0.0,
           {{"L", "S2", 1.0, 2.5, 2000, 4.0}, {"S1", "__close__", 1.0, 4.0, 3000, 4.0}}, 2);
    // L9d: the resting S2 closes L2 on the later bar at its own level.
    expect("ord v6 later low 0.5 (S2 closes L2)", 6, true, 2, 0.0,
           {{"L", "S1", 1.0, 4.0, 2000, 4.0}, {"L2", "S2", 1.0, 1.0, 2000, 5.5}}, 1);
    std::printf("test_l9b_open_marketable_order: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
