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
 *
 * The LIMIT-leg cells below pin finding 278 seed (b)
 * (rhyme17-trendline-and-horizontal-breakout, six tape events): on a
 * reversal fill bar TV still honors the STANDING prior-bar strategy.exit
 * whose levels were computed from the OLD (reversed-away) position's avg
 * price. A limit already marketable at the fill bar's open fills AT THE
 * OPEN — a duration-0 PnL-0 trade for the new position — and the re-priced
 * bracket issued at that bar's close governs afterwards. Equality with the
 * open is marketable (2025-04-07 14:00 UTC: entry and exit both 1549.51).
 *
 * Scope: these cells exercise exit ORDER lifecycle only (when a standing
 * strategy.exit may fill on its parent's fill bar). They do not touch the
 * reverted same-bar position_size visibility class, the #146 same-tick
 * close+reverse sequencing kernel, or ordinary non-reversal exit re-issues
 * (see the OngoingPositionReissue control).
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

// ── LIMIT-leg cells (finding 278 seed (b), rhyme17 stale-exit family) ──

enum class LimitCell {
    FlatLongLimit,        // flat parent, TP limit below the fill open
    FlatShortLimit,       // flat parent, TP limit above the fill open
    ReversalLongLimit,    // short→long reversal, stale old-avg TP below open
    ReversalShortLimit,   // long→short reversal, stale old-avg TP above open
    ReversalLongLimitEq,  // rhyme17 2025-04-07 shape: limit == open exactly
    ReversalLongLimitPostOpen,   // correctly-sided limit, fills later at level
    ReversalShortLimitPostOpen,  // correctly-sided limit, fills later at level
    ReversalDualMarketable,      // stop AND limit marketable: no open scratch
};

class PrearmedLimitBracketProbe final : public BacktestEngine {
public:
    explicit PrearmedLimitBracketProbe(LimitCell cell) : cell_(cell) {
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
        const bool flat_parent = cell_ == LimitCell::FlatLongLimit
                              || cell_ == LimitCell::FlatShortLimit;
        const bool opens_long = cell_ != LimitCell::FlatShortLimit
                             && cell_ != LimitCell::ReversalShortLimit
                             && cell_ != LimitCell::ReversalShortLimitPostOpen;

        double limit_px;
        double stop_px;
        switch (cell_) {
            case LimitCell::FlatLongLimit:
            case LimitCell::ReversalLongLimit:
                // Marketable at the O=100 fill open (open >= limit) with a
                // correctly-sided, non-gapped stop sibling.
                limit_px = 95.0;
                stop_px = 90.0;
                break;
            case LimitCell::FlatShortLimit:
            case LimitCell::ReversalShortLimit:
                limit_px = 105.0;
                stop_px = 110.0;
                break;
            case LimitCell::ReversalLongLimitEq:
                // Equality is marketable (rhyme17 event 1: open == limit).
                limit_px = 100.0;
                stop_px = 90.0;
                break;
            case LimitCell::ReversalLongLimitPostOpen:
                limit_px = 110.0;   // above open: not marketable at open
                stop_px = 80.0;     // out of the bar's range: limit leg fills
                break;
            case LimitCell::ReversalShortLimitPostOpen:
                limit_px = 90.0;    // below open: not marketable at open
                stop_px = 120.0;    // out of the bar's range: limit leg fills
                break;
            case LimitCell::ReversalDualMarketable:
                limit_px = 95.0;    // marketable at open ...
                stop_px = 105.0;    // ... and the stop is gapped too
                break;
        }

        if (flat_parent && bar_index_ == 0) {
            strategy_entry("E", opens_long, kNaN, kNaN, 1.0, "flat parent");
            strategy_exit("X", "E", limit_px, stop_px,
                          kNaN, kNaN, kNaN, 100.0, "prearmed limit");
            return;
        }

        if (!flat_parent && bar_index_ == 0) {
            strategy_entry("OLD", !opens_long, kNaN, kNaN, 1.0, "seed");
            return;
        }

        if (!flat_parent && bar_index_ == 1) {
            strategy_entry("E", opens_long, kNaN, kNaN, 1.0, "reverse parent");
            strategy_exit("X", "E", limit_px, stop_px,
                          kNaN, kNaN, kNaN, 100.0, "stale old-avg bracket");
        }
    }

private:
    LimitCell cell_;
};

static void check_flat_limit_gap(LimitCell cell, bool is_long) {
    PrearmedLimitBracketProbe probe(cell);
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
    // Fill books at the OPEN (limit-or-better), not at the limit level.
    CHECK(near(t.entry_price, 100.0));
    CHECK(near(t.exit_price, 100.0));
    CHECK(near(t.qty, 1.0));
    CHECK(near(t.pnl, 0.0));
    CHECK(t.exit_id == "X");
    CHECK(probe.is_flat());
    CHECK(near(probe.live_qty(), 0.0));
}

static void check_reversal_limit(LimitCell cell, bool new_is_long,
                                 bool post_open) {
    PrearmedLimitBracketProbe probe(cell);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 101.0, 99.0, 100.0),
        // Marketable cells scratch at O=100 (duration-0, PnL-0). The
        // post-open controls carry a correctly-sided limit at 110/90,
        // touched later by H=110 / L=90 and filled at that level.
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
               post_open ? (new_is_long ? 110.0 : 90.0) : 100.0));
    CHECK(near(fresh.qty, 1.0));
    CHECK(near(fresh.pnl, post_open ? 10.0 : 0.0));
    CHECK(fresh.exit_id == "X");
    CHECK(probe.is_flat());
    CHECK(near(probe.live_qty(), 0.0));
}

// Dual-marketable bracket (stop gapped AND limit marketable at the open):
// stays OFF the open-scratch path — no duration-0 trade on the entry bar.
// The wrong-side stop is skipped on the entry bar and the order fires via
// the ordinary resting-order gap on the NEXT bar's open (pre-existing
// behavior, unchanged by the limit-leg extension).
static void check_reversal_dual_marketable_holds_entry_bar() {
    PrearmedLimitBracketProbe probe(LimitCell::ReversalDualMarketable);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 101.0, 99.0, 100.0),
        bar(3'000, 100.0, 101.0, 99.0, 100.0),
        bar(4'000, 100.0, 101.0, 99.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 2);
    if (probe.trade_count() != 2) return;
    const Trade& fresh = probe.get_trade(1);
    CHECK(fresh.entry_bar_index == 2);
    CHECK(fresh.exit_bar_index == 3);   // NOT the entry bar
    CHECK(near(fresh.entry_price, 100.0));
    CHECK(near(fresh.exit_price, 100.0));
}

// Ordinary non-reversal exit re-issue control: the position has been open
// since an EARLIER bar when a fresh strategy.exit with a marketable limit is
// issued. The prearmed oracle must not treat it as a parent-fill-bar scratch
// (position_open_bar_ gate): the exit fills on its ordinary next-bar
// resting-order path and the trade keeps its original entry bar.
class OngoingPositionReissue final : public BacktestEngine {
public:
    OngoingPositionReissue() {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
    }

    double live_qty() const { return position_qty_; }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("E", true, kNaN, kNaN, 1.0, "hold");
        } else if (bar_index_ == 2) {
            strategy_exit("X", "E", /*limit=*/99.0, kNaN,
                          kNaN, kNaN, kNaN, 100.0, "re-issue");
        }
    }
};

static void check_ongoing_position_reissue_keeps_entry() {
    OngoingPositionReissue probe;
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 100.5, 99.5, 100.0),
        bar(2'000, 100.0, 100.5, 99.5, 100.0),
        bar(3'000, 100.0, 100.5, 99.5, 100.0),
        bar(4'000, 100.0, 100.5, 99.5, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.entry_bar_index == 1);   // original entry preserved
    CHECK(t.exit_bar_index == 3);    // fills on the re-issue's next bar
    CHECK(near(t.entry_price, 100.0));
    CHECK(near(t.exit_price, 100.0));
    CHECK(near(probe.live_qty(), 0.0));
}

class PartialFlatLimitBracket final : public BacktestEngine {
public:
    explicit PartialFlatLimitBracket(double exit_qty) : exit_qty_(exit_qty) {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 1;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ != 0) return;
        strategy_entry("E", true, kNaN, kNaN, 1.0);
        strategy_exit("X", "E", /*limit=*/95.0, /*stop=*/90.0,
                      kNaN, kNaN, kNaN, 100.0, "partial limit", exit_qty_);
    }

    double live_qty() const { return position_qty_; }

private:
    double exit_qty_;
};

static void check_partial_limit_does_not_scratch_parent_open() {
    PartialFlatLimitBracket probe(0.5);
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

    // LIMIT-leg cells (finding 278 seed (b), rhyme17 stale-exit family).
    check_flat_limit_gap(LimitCell::FlatLongLimit, true);
    check_flat_limit_gap(LimitCell::FlatShortLimit, false);
    check_reversal_limit(LimitCell::ReversalLongLimit, true, false);
    check_reversal_limit(LimitCell::ReversalShortLimit, false, false);
    check_reversal_limit(LimitCell::ReversalLongLimitEq, true, false);
    check_reversal_limit(LimitCell::ReversalLongLimitPostOpen, true, true);
    check_reversal_limit(LimitCell::ReversalShortLimitPostOpen, false, true);
    check_reversal_dual_marketable_holds_entry_bar();
    check_ongoing_position_reissue_keeps_entry();
    check_partial_limit_does_not_scratch_parent_open();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
