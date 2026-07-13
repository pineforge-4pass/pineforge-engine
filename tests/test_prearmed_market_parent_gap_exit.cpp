/*
 * A valid strategy.exit bracket can be armed before its from_entry MARKET
 * parent fills. When the parent opens at the next bar's open and that open has
 * already breached the retained stop, TradingView scratches the new position
 * at the same open. This applies to parents placed from true flat and to
 * opposite-side market reversals. Correctly-sided stops continue to walk the
 * remaining entry-bar path and fill at their level.
 *
 * The six cells mirror the clean-room TradingView probe
 * order-market-reversal-resting-bracket-gap-01 (A-F).
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
    FlatLongGap,
    FlatShortGap,
    ReversalLongGap,
    ReversalShortGap,
    ReversalLongPostOpen,
    ReversalShortPostOpen,
};

class PrearmedMarketBracketProbe final : public BacktestEngine {
public:
    explicit PrearmedMarketBracketProbe(Cell cell) : cell_(cell) {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
    }

    double live_qty() const { return position_qty_; }
    bool is_flat() const { return position_side_ == PositionSide::FLAT; }

    void on_bar(const Bar&) override {
        const bool flat_parent = cell_ == Cell::FlatLongGap
                              || cell_ == Cell::FlatShortGap;
        const bool opens_long = cell_ == Cell::FlatLongGap
                             || cell_ == Cell::ReversalLongGap
                             || cell_ == Cell::ReversalLongPostOpen;

        if (flat_parent && bar_index_ == 0) {
            strategy_entry("E", opens_long, kNaN, kNaN, 1.0, "flat parent");
            strategy_exit("X", "E", opens_long ? 120.0 : 80.0,
                          /*stop=*/opens_long ? 105.0 : 95.0,
                          kNaN, kNaN, kNaN, 100.0, "prearmed gap");
            return;
        }

        if (!flat_parent && bar_index_ == 0) {
            strategy_entry("OLD", !opens_long, kNaN, kNaN, 1.0, "seed");
            return;
        }

        if (!flat_parent && bar_index_ == 1) {
            strategy_entry("E", opens_long, kNaN, kNaN, 1.0, "reverse parent");
            const bool post_open = cell_ == Cell::ReversalLongPostOpen
                                || cell_ == Cell::ReversalShortPostOpen;
            const double stop = opens_long
                ? (post_open ? 95.0 : 105.0)
                : (post_open ? 105.0 : 95.0);
            strategy_exit("X", "E", opens_long ? 120.0 : 80.0, stop,
                          kNaN, kNaN, kNaN, 100.0, "prearmed stop");
        }
    }

private:
    Cell cell_;
};

static Bar bar(int64_t ts, double o, double h, double l, double c) {
    return {o, h, l, c, 1'000.0, ts};
}

static void check_flat_gap(Cell cell, bool is_long) {
    PrearmedMarketBracketProbe probe(cell);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 102.0, 98.0, 100.0),
        bar(3'000, 100.0, 102.0, 98.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.is_long == is_long);
    CHECK(t.entry_bar_index == 1);
    CHECK(t.exit_bar_index == 1);
    CHECK(near(t.entry_price, 100.0));
    CHECK(near(t.exit_price, 100.0));
    CHECK(near(t.qty, 1.0));
    CHECK(near(t.pnl, 0.0));
    CHECK(t.exit_id == "X");
    CHECK(probe.is_flat());
    CHECK(near(probe.live_qty(), 0.0));
}

static void check_reversal(Cell cell, bool new_is_long, bool post_open) {
    PrearmedMarketBracketProbe probe(cell);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 101.0, 99.0, 100.0),
        // Gap cells have a wrong-side stop and must scratch at O=100. The
        // post-open controls have a correctly-sided stop at 95/105, crossed
        // later by L=90 or H=110 and filled at that level.
        bar(3'000, 100.0, 110.0, 90.0, 100.0),
        bar(4'000, 100.0, 110.0, 90.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 2);
    if (probe.trade_count() != 2) return;
    const Trade& old = probe.get_trade(0);
    const Trade& fresh = probe.get_trade(1);
    CHECK(old.is_long != new_is_long);
    CHECK(fresh.is_long == new_is_long);
    CHECK(fresh.entry_bar_index == 2);
    CHECK(fresh.exit_bar_index == 2);
    CHECK(near(fresh.entry_price, 100.0));
    CHECK(near(fresh.exit_price,
               post_open ? (new_is_long ? 95.0 : 105.0) : 100.0));
    CHECK(near(fresh.qty, 1.0));
    CHECK(near(fresh.pnl,
               post_open ? -5.0 : 0.0));
    CHECK(fresh.exit_id == "X");
    CHECK(probe.is_flat());
    CHECK(near(probe.live_qty(), 0.0));
}

class PartialFlatBracket final : public BacktestEngine {
public:
    explicit PartialFlatBracket(double exit_qty) : exit_qty_(exit_qty) {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        strategy_entry("E", true, kNaN, kNaN, 1.0);
        strategy_exit("X", "E", 120.0, 105.0,
                      kNaN, kNaN, kNaN, 100.0, "partial", exit_qty_);
    }

    double live_qty() const { return position_qty_; }

private:
    double exit_qty_;
};

static void check_partial_qty_does_not_scratch_parent_open(double exit_qty) {
    PartialFlatBracket probe(exit_qty);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 102.0, 98.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 0);
    CHECK(near(probe.live_qty(), 1.0));
}

class MultipleFlatParents final : public BacktestEngine {
public:
    MultipleFlatParents() {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 2;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        strategy_entry("E", true, kNaN, kNaN, 1.0);
        strategy_entry("F", true, kNaN, kNaN, 1.0);
        strategy_exit("X", "E", 120.0, 105.0,
                      kNaN, kNaN, kNaN, 100.0, "multi-parent");
    }

    double live_qty() const { return position_qty_; }
};

static void check_multiple_market_parents_do_not_share_scratch() {
    MultipleFlatParents probe;
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 102.0, 98.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 0);
    CHECK(near(probe.live_qty(), 2.0));
}

int main() {
    std::printf("prearmed MARKET-parent bracket gap exits\n");

    check_flat_gap(Cell::FlatLongGap, true);
    check_flat_gap(Cell::FlatShortGap, false);
    check_reversal(Cell::ReversalLongGap, true, false);
    check_reversal(Cell::ReversalShortGap, false, false);
    check_reversal(Cell::ReversalLongPostOpen, true, true);
    check_reversal(Cell::ReversalShortPostOpen, false, true);
    check_partial_qty_does_not_scratch_parent_open(0.5);
    // kFullQtyEps is wider than the engine's actual flattening threshold. A
    // near-full literal that would leave live dust must remain off this path.
    check_partial_qty_does_not_scratch_parent_open(0.9999999995);
    check_multiple_market_parents_do_not_share_scratch();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
