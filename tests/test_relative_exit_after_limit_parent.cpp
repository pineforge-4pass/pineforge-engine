/*
 * A relative strategy.exit can be armed while flat beside its pending LIMIT
 * from_entry parent.  If the parent fills away from the next bar's open, the
 * child becomes live at that fill coordinate: later path touches may fill it,
 * while target touches that happened before the parent must not be replayed.
 *
 * These direction-symmetric cells cover the lifecycle that the older market-
 * parent relative-exit test cannot expose.  A MARKET parent and its unresolved
 * child share the open phase; a non-gap LIMIT parent does not.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Cell {
    LongPostEntryStop,
    ShortPostEntryStop,
    LongPreEntryTarget,
    ShortPreEntryTarget,
};

class RelativeLimitBracketProbe final : public BacktestEngine {
public:
    explicit RelativeLimitBracketProbe(Cell cell) : cell_(cell) {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        syminfo_mintick_ = 1.0;
        pyramiding_ = 1;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;

        const bool is_long = cell_ == Cell::LongPostEntryStop
                          || cell_ == Cell::LongPreEntryTarget;
        const bool target_only = cell_ == Cell::LongPreEntryTarget
                              || cell_ == Cell::ShortPreEntryTarget;
        strategy_entry("E", is_long, /*limit=*/100.0, /*stop=*/kNaN,
                       /*qty=*/1.0, "non-gap limit parent");
        strategy_exit("X", "E", /*limit=*/kNaN, /*stop=*/kNaN,
                      /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                      /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                      target_only ? "pre-entry target" : "post-entry stop",
                      /*qty=*/kNaN, /*oca_name=*/"",
                      /*profit_ticks=*/target_only ? 5.0 : kNaN,
                      /*loss_ticks=*/target_only ? kNaN : 5.0);
    }

private:
    Cell cell_;
};

class MultiChildFenceProbe final : public BacktestEngine {
public:
    MultiChildFenceProbe() {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        syminfo_mintick_ = 1.0;
        pyramiding_ = 1;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;

        strategy_entry("E", true, /*limit=*/100.0, /*stop=*/kNaN,
                       /*qty=*/1.0, "limit parent");
        strategy_exit("T", "E", /*limit=*/kNaN, /*stop=*/kNaN,
                      /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                      /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                      "target child", /*qty=*/1.0, /*oca_name=*/"",
                      /*profit_ticks=*/5.0, /*loss_ticks=*/kNaN);
        strategy_exit("S", "E", /*limit=*/kNaN, /*stop=*/kNaN,
                      /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                      /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                      "far stop child", /*qty=*/1.0, /*oca_name=*/"",
                      /*profit_ticks=*/kNaN, /*loss_ticks=*/50.0);
    }
};

class MultiParentFenceProbe final : public BacktestEngine {
public:
    MultiParentFenceProbe() {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        syminfo_mintick_ = 1.0;
        pyramiding_ = 2;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;

        strategy_entry("E1", true, /*limit=*/100.0, /*stop=*/kNaN,
                       /*qty=*/1.0, "first parent");
        strategy_exit("X1", "E1", /*limit=*/kNaN, /*stop=*/kNaN,
                      /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                      /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                      "first stop", /*qty=*/1.0, /*oca_name=*/"",
                      /*profit_ticks=*/kNaN, /*loss_ticks=*/2.0);
        strategy_entry("E2", true, /*limit=*/95.0, /*stop=*/kNaN,
                       /*qty=*/1.0, "second parent");
        strategy_exit("X2", "E2", /*limit=*/kNaN, /*stop=*/kNaN,
                      /*trail_points=*/kNaN, /*trail_offset=*/kNaN,
                      /*trail_price=*/kNaN, /*qty_percent=*/100.0,
                      "second target", /*qty=*/1.0, /*oca_name=*/"",
                      /*profit_ticks=*/5.0, /*loss_ticks=*/kNaN);
    }
};

static Bar bar(int64_t ts, double o, double h, double l, double c) {
    return {o, h, l, c, 1'000.0, ts};
}

static void check_cell(Cell cell, bool is_long, bool target_only) {
    RelativeLimitBracketProbe probe(cell);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        is_long
            // O-H-L-C: target 105 is pre-parent; limit 100 then stop 95.
            ? bar(2'000, 105.0, 110.0, 90.0, 95.0)
            // O-L-H-C: target 95 is pre-parent; limit 100 then stop 105.
            : bar(2'000, 95.0, 110.0, 90.0, 105.0),
        is_long
            ? bar(3'000, 95.0, 106.0, 94.0, 105.0)
            : bar(3'000, 105.0, 106.0, 94.0, 95.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    const char* label = is_long
        ? (target_only ? "long-pre-target" : "long-post-stop")
        : (target_only ? "short-pre-target" : "short-post-stop");
    std::printf("  %s: trades=%d", label, probe.trade_count());
    if (probe.trade_count() > 0) {
        const Trade& observed = probe.get_trade(0);
        std::printf(" entry=%d@%.2f exit=%d@%.2f",
                    observed.entry_bar_index, observed.entry_price,
                    observed.exit_bar_index, observed.exit_price);
    }
    std::printf("\n");

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;

    const Trade& trade = probe.get_trade(0);
    CHECK(trade.is_long == is_long);
    CHECK(trade.entry_bar_index == 1);
    CHECK(trade.exit_bar_index == (target_only ? 2 : 1));
    CHECK(near(trade.entry_price, 100.0));
    CHECK(near(trade.exit_price, is_long
        ? (target_only ? 105.0 : 95.0)
        : (target_only ? 95.0 : 105.0)));
    CHECK(near(trade.pnl, target_only ? 5.0 : -5.0));
    CHECK(trade.exit_id == "X");
}

static void check_multi_child_fence() {
    MultiChildFenceProbe probe;
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        // O-H-L-C: target 105 is elapsed before the parent reaches 100.
        bar(2'000, 105.0, 110.0, 90.0, 95.0),
        bar(3'000, 100.0, 106.0, 99.0, 105.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    std::printf("  multi-child fence: trades=%d", probe.trade_count());
    if (probe.trade_count() > 0) {
        const Trade& observed = probe.get_trade(0);
        std::printf(" entry=%d@%.2f exit=%d@%.2f id=%s",
                    observed.entry_bar_index, observed.entry_price,
                    observed.exit_bar_index, observed.exit_price,
                    observed.exit_id.c_str());
    }
    std::printf("\n");

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;

    const Trade& trade = probe.get_trade(0);
    CHECK(trade.is_long);
    CHECK(trade.entry_bar_index == 1);
    CHECK(trade.exit_bar_index == 2);
    CHECK(near(trade.entry_price, 100.0));
    CHECK(near(trade.exit_price, 105.0));
    CHECK(trade.exit_id == "T");
}

static void check_multi_parent_fence() {
    MultiParentFenceProbe probe;
    std::vector<Bar> bars = {
        bar(1'000, 105.0, 106.0, 104.0, 105.0),
        // O-H-L-C: E1 100 -> X1 98 -> E2 95. The second parent must not
        // leapfrog X1 merely because both parents were initially phase 1.
        bar(2'000, 105.0, 110.0, 90.0, 95.0),
        bar(3'000, 95.0, 105.0, 94.0, 104.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    std::printf("  multi-parent fence: trades=%d\n", probe.trade_count());
    for (int i = 0; i < probe.trade_count(); ++i) {
        const Trade& observed = probe.get_trade(i);
        std::printf("    %s entry=%d@%.2f exit=%d@%.2f id=%s\n",
                    observed.entry_id.c_str(), observed.entry_bar_index,
                    observed.entry_price, observed.exit_bar_index,
                    observed.exit_price, observed.exit_id.c_str());
    }

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 2);
    if (probe.trade_count() != 2) return;

    const Trade& first = probe.get_trade(0);
    const Trade& second = probe.get_trade(1);
    CHECK(first.entry_id == "E1");
    CHECK(first.entry_bar_index == 1);
    CHECK(first.exit_bar_index == 1);
    CHECK(near(first.entry_price, 100.0));
    CHECK(near(first.exit_price, 98.0));
    CHECK(first.exit_id == "X1");
    CHECK(second.entry_id == "E2");
    CHECK(second.entry_bar_index == 1);
    CHECK(second.exit_bar_index == 2);
    CHECK(near(second.entry_price, 95.0));
    CHECK(near(second.exit_price, 100.0));
    CHECK(second.exit_id == "X2");
}

int main() {
    std::printf("relative exit after non-gap LIMIT parent\n");
    check_cell(Cell::LongPostEntryStop, true, false);
    check_cell(Cell::ShortPostEntryStop, false, false);
    check_cell(Cell::LongPreEntryTarget, true, true);
    check_cell(Cell::ShortPreEntryTarget, false, true);
    check_multi_child_fence();
    check_multi_parent_fence();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
