// R4-D L9e P0-N1: a non-NaN limit/stop of 0.0 is a present price level
// (ab9714be pine_strategy_commands.cpp:533-537). Literals are the ab9714be
// output of EV/tasks/r4-d/opus-delta3-probes/zero/{probe,probe2}.cpp
// (base.txt / base2.txt).
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

class EntryHost : public source::PineStrategyHost {
public:
    int variant = 0;
    explicit EntryHost(int v, bool pooc) : variant(v) {
        configure_pine_strategy(cfg(100000, (int)QtyType::FIXED, 1.0, 1, pooc));
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        if (variant == 0) strategy_order("O", true, kNaN, kNaN, 0.0);
        if (variant == 1) strategy_entry("E", true, kNaN, 0.0);
        if (variant == 2) strategy_order("O", true, kNaN, 0.0);
        if (variant == 3) strategy_entry("E", true, 0.0);
    }
};

class ExitHost : public source::PineStrategyHost {
public:
    int variant = 0;
    explicit ExitHost(int v) : variant(v) {
        configure_pine_strategy(cfg(100000, (int)QtyType::FIXED, 1.0, 1, false));
    }
    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) {
            if (variant == 0) strategy_entry("E", false, kNaN, 0.0);
            if (variant == 1) strategy_entry("E", false, 0.0);
            if (variant >= 2) strategy_entry("E", true);
        }
        if (i == 1) {
            if (variant == 2) strategy_exit("X", "E", 0.0, kNaN);
            if (variant == 3) strategy_exit("X", "E", kNaN, 0.0);
            if (variant == 4) strategy_exit("X", "E", 0.0, 0.0);
            if (variant == 5) strategy_exit("X", "E", kNaN, kNaN, 0.0, 0.0);
        }
    }
};

struct TradeLiteral {
    const char* entry;
    const char* exit;
    double qty;
    double exit_price;
};

void expect_entry(const char* tag, int variant, bool pooc, int trades, double pos) {
    EntryHost host(variant, pooc);
    std::vector<Bar> bars = {
        mk(1000, 4, 4.5, 4, 4.5),
        mk(2000, 5, 6, 3, 5),
        mk(3000, 6, 6, 6, 6),
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    std::printf("%s\n", tag);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == trades);
    CHECK(near(host.live_position_size(), pos));
}

void expect_exit(const char* tag, int variant, int trades, double pos,
                 std::initializer_list<TradeLiteral> rows) {
    ExitHost host(variant);
    std::vector<Bar> bars = {
        mk(1000, 4, 4.5, 4, 4.5),
        mk(2000, 5, 6, 3, 5),
        mk(3000, 6, 6, 6, 6),
        mk(4000, 6, 7, 5, 6),
    };
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
    // probe.cpp / base.txt: buy stop 0 fills; buy limit 0 rests.
    expect_entry("pooc0 v0 (buy stop 0 fills)", 0, false, 0, 1.0);
    expect_entry("pooc0 v1 (buy stop 0 fills)", 1, false, 0, 1.0);
    expect_entry("pooc0 v2 (buy limit 0 rests)", 2, false, 0, 0.0);
    expect_entry("pooc0 v3 (buy limit 0 rests)", 3, false, 0, 0.0);
    expect_entry("pooc1 v0 (buy stop 0 fills)", 0, true, 0, 1.0);
    expect_entry("pooc1 v1 (buy stop 0 fills)", 1, true, 0, 1.0);
    expect_entry("pooc1 v2 (buy limit 0 rests)", 2, true, 0, 0.0);
    expect_entry("pooc1 v3 (buy limit 0 rests)", 3, true, 0, 0.0);

    // probe2.cpp / base2.txt
    expect_exit("v0 (sell stop 0 never fills)", 0, 0, 0.0, {});
    expect_exit("v1 (sell limit 0 fills)", 1, 0, -1.0, {});
    expect_exit("v2 (exit limit 0 fills)", 2, 1, 0.0, {{"E", "X", 1.0, 6.0}});
    expect_exit("v3 (exit stop 0 never fills)", 3, 0, 1.0, {});
    expect_exit("v4 (exit limit+stop 0 fills via limit)", 4, 1, 0.0, {{"E", "X", 1.0, 6.0}});
    expect_exit("v5 (trail 0/0 fills)", 5, 1, 0.0, {{"E", "X", 1.0, 6.0}});

    std::printf("test_l9e_zero_price_presence: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
