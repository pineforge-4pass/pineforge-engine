/*
 * Prearmed strategy.exit brackets resolve on their parent's FILL bar.
 *
 * Three tape-pinned shapes, all order-lifecycle semantics:
 *
 *  (1a) DUAL-MARKETABLE bracket. bprakaash-new-era-strategy-1-0
 *       (OANDA:EURUSD 15m, 2025-07-03 / 07-24 / 08-07 / 09-09 13:30Z):
 *       strategy.entry("Short", qty=1) + strategy.exit("TP/SL 1", "Short",
 *       qty=1, stop=sl, limit=target) armed on the signal bar with sl BELOW
 *       the close (so target lands above it). At the fill open both legs are
 *       marketable (stop 1.17528 < open 1.17646 < limit 1.17879; on 08-07
 *       stop == limit == open). TV fills the entry at the open and one leg
 *       at the same open: exit px == entry px, duration 0, PnL 0. Before
 *       this pin the engine held dual-marketable brackets off the open
 *       scratch ("no tape exemplar") and gap-filled them the next bar.
 *
 *  (1b) TRAIL-carrying leg. stevenygabbyperez-fast-scalper-with-stops
 *       (NASDAQ:AAPL 15m, 2025-04-03 / 2026-04-27 13:30Z):
 *       strategy.exit(stop=close*0.99, trail_points=...) armed with a MARKET
 *       entry; the RTH open gaps below the stop. TV: entry + 'Exit Long' at
 *       the open (205.54 / 266.09), PnL 0. The trail leg is dormant until
 *       activation and does not change the breached stop's fill.
 *
 *  (2)  RELATIVE-TICKS bracket of a parent that fills INTRABAR.
 *       quantbyboji-nq-hma-midday-strategy (OANDA:EURUSD 15m, 2025-08-22
 *       18:15Z): resting limit 1.17323 fills mid-path (open 1.17356), the
 *       loss leg binds to the fill price and resolves on the remaining path
 *       of the same bar (exit 1.17322). 140/141 sibling exits whose parent
 *       filled at the open already matched; only the mid-path fill deferred
 *       the child to the next bar's open.
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

static Bar bar(int64_t ts, double o, double h, double l, double c) {
    return {o, h, l, c, 1'000.0, ts};
}

// ── (1a) dual-marketable bracket ───────────────────────────────────────

enum class DualCell {
    ShortBothInside,    // stop below the open, limit above it (07-03 shape)
    ShortBothEqualOpen, // stop == limit == open (08-07 shape)
    LongBothInside,     // mirror
    ShortStopOnlyGap,   // control: single-leg gap keeps its existing path
};

class DualMarketableBracket final : public BacktestEngine {
public:
    DualMarketableBracket(DualCell cell, bool reversal)
        : cell_(cell), reversal_(reversal) {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
    }

    bool opens_long() const { return cell_ == DualCell::LongBothInside; }
    double live_qty() const { return position_qty_; }
    bool is_flat() const { return position_side_ == PositionSide::FLAT; }

    void on_bar(const Bar&) override {
        const int arm_bar = reversal_ ? 1 : 0;
        if (reversal_ && bar_index_ == 0) {
            strategy_entry("OLD", !opens_long(), kNaN, kNaN, 1.0, "seed");
            return;
        }
        if (bar_index_ != arm_bar) return;
        double stop_px;
        double limit_px;
        switch (cell_) {
            case DualCell::ShortBothInside:
                stop_px = 95.0;    // short buy-stop below the 100 open
                limit_px = 110.0;  // short buy-limit above the 100 open
                break;
            case DualCell::ShortBothEqualOpen:
                stop_px = 100.0;
                limit_px = 100.0;
                break;
            case DualCell::LongBothInside:
                stop_px = 105.0;   // long sell-stop above the 100 open
                limit_px = 90.0;   // long sell-limit below the 100 open
                break;
            case DualCell::ShortStopOnlyGap:
                stop_px = 95.0;
                limit_px = 80.0;   // not marketable at the open
                break;
        }
        // bprakaash shape: explicit qty on both the entry and the exit.
        strategy_entry(opens_long() ? "Long" : "Short", opens_long(),
                       kNaN, kNaN, 1.0, "signal");
        strategy_exit("TP/SL 1", opens_long() ? "Long" : "Short",
                      limit_px, stop_px,
                      kNaN, kNaN, kNaN, /*qty_percent=*/100.0, "bracket",
                      /*qty=*/1.0);
    }

private:
    DualCell cell_;
    bool reversal_;
};

static void check_dual_marketable_scratches_at_open(DualCell cell,
                                                    bool reversal) {
    DualMarketableBracket probe(cell, reversal);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 100.0, 101.0, 99.0, 100.0),
        bar(3'000, 100.0, 101.0, 99.0, 100.0),
        bar(4'000, 100.0, 101.0, 99.0, 100.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    const int fill_bar = reversal ? 2 : 1;
    const int expected_trades = reversal ? 2 : 1;
    CHECK(probe.trade_count() == expected_trades);
    if (probe.trade_count() != expected_trades) return;
    const Trade& t = probe.get_trade(expected_trades - 1);
    CHECK(t.is_long == probe.opens_long());
    CHECK(t.entry_bar_index == fill_bar);
    CHECK(t.exit_bar_index == fill_bar);
    CHECK(near(t.entry_price, 100.0));
    CHECK(near(t.exit_price, 100.0));
    CHECK(near(t.qty, 1.0));
    CHECK(near(t.pnl, 0.0));
    CHECK(t.exit_id == "TP/SL 1");
    CHECK(probe.is_flat());
    CHECK(near(probe.live_qty(), 0.0));
}

// Control: a correctly-sided explicit-qty bracket keeps its ordinary path
// (the 265 bprakaash trades that already matched).
static void check_explicit_qty_bracket_no_gap_control() {
    DualMarketableBracket probe(DualCell::ShortStopOnlyGap, false);
    std::vector<Bar> bars = {
        bar(1'000, 100.0, 101.0, 99.0, 100.0),
        bar(2'000, 92.0, 93.0, 91.0, 92.0),    // opens below stop 95: no gap
        bar(3'000, 92.0, 93.0, 91.0, 92.0),
        bar(4'000, 92.0, 97.0, 91.0, 92.0),    // stop 95 crossed
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.entry_bar_index == 1);
    CHECK(t.exit_bar_index == 3);
    CHECK(near(t.entry_price, 92.0));
    CHECK(near(t.exit_price, 95.0));
}

// ── (1b) trail-carrying leg ────────────────────────────────────────────

class TrailBracket final : public BacktestEngine {
public:
    TrailBracket(bool opens_long, bool reversal, bool percent_sizing)
        : opens_long_(opens_long), reversal_(reversal) {
        initial_capital_ = 100'000.0;
        if (percent_sizing) {
            default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
            default_qty_value_ = 100.0;
        } else {
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
        }
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
    }

    double live_qty() const { return position_qty_; }
    bool is_flat() const { return position_side_ == PositionSide::FLAT; }

    void on_bar(const Bar& b) override {
        const int arm_bar = reversal_ ? 1 : 0;
        if (reversal_ && bar_index_ == 0) {
            strategy_entry("OLD", !opens_long_, kNaN, kNaN, kNaN, "seed");
            return;
        }
        if (bar_index_ != arm_bar) return;
        // stevenygabbyperez shape: stop from the signal close plus a
        // trail_points activation, default (percent) sizing.
        strategy_entry(opens_long_ ? "Long" : "Short", opens_long_,
                       kNaN, kNaN, kNaN, "signal");
        strategy_exit(opens_long_ ? "Exit Long" : "Exit Short",
                      opens_long_ ? "Long" : "Short",
                      /*limit=*/kNaN,
                      /*stop=*/opens_long_ ? b.close * 0.99 : b.close * 1.01,
                      /*trail_points=*/b.close * 0.02 / syminfo_mintick_,
                      kNaN, kNaN, 100.0, "bracket");
    }

private:
    bool opens_long_;
    bool reversal_;
};

static void check_trail_stop_gap(bool opens_long, bool reversal,
                                 bool percent_sizing) {
    TrailBracket probe(opens_long, reversal, percent_sizing);
    std::vector<Bar> bars = {
        bar(1'000, 224.0, 224.5, 223.5, 224.0),
        bar(2'000, 224.0, 224.5, 223.5, 224.0),
        bar(3'000, 224.0, 224.5, 223.5, 224.0),
        bar(4'000, 224.0, 224.5, 223.5, 224.0),
    };
    // -8% gap through the 0.99*close stop (long) / +8% through the
    // 1.01*close stop (short).
    const int fill_bar = reversal ? 2 : 1;
    const double open = opens_long ? 205.54 : 242.0;
    bars[fill_bar] = bar(bars[fill_bar].timestamp, open,
                         open + 2.0, open - 3.0, open - 2.6);
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    // The 100%-equity seed of the percent cell also books a same-bar
    // margin-call slice against its own adverse tick; the scratch under
    // test is always the LAST trade.
    const int expected_trades = reversal ? 2 : 1;
    CHECK(probe.trade_count() >= expected_trades);
    if (probe.trade_count() < expected_trades) return;
    const Trade& t = probe.get_trade(probe.trade_count() - 1);
    CHECK(t.is_long == opens_long);
    CHECK(t.entry_bar_index == fill_bar);
    CHECK(t.exit_bar_index == fill_bar);
    CHECK(near(t.entry_price, open));
    CHECK(near(t.exit_price, open));
    CHECK(near(t.pnl, 0.0));
    CHECK(t.exit_id == (opens_long ? "Exit Long" : "Exit Short"));
    CHECK(probe.is_flat());
    CHECK(near(probe.live_qty(), 0.0));
}

// Control: no gap through the stop — the stop leg walks the entry-bar path
// and fills at its level (the 11 stevenygabbyperez same-bar stops that
// already matched), the trail never activates.
static void check_trail_stop_intrabar_control() {
    TrailBracket probe(true, false, false);
    std::vector<Bar> bars = {
        bar(1'000, 224.0, 224.5, 223.5, 224.0),
        // stop = 221.76; open above it, low below it.
        bar(2'000, 224.0, 224.5, 220.0, 221.0),
        bar(3'000, 221.0, 222.0, 220.0, 221.0),
    };
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.entry_bar_index == 1);
    CHECK(t.exit_bar_index == 1);
    CHECK(near(t.entry_price, 224.0));
    CHECK(near(t.exit_price, 221.76, 1e-6));
}

// ── (2) relative-ticks bracket, parent fills intrabar ─────────────────

class IntrabarLimitParentTicks final : public BacktestEngine {
public:
    explicit IntrabarLimitParentTicks(double loss_ticks, double profit_ticks,
                                      bool reissue_every_bar)
        : loss_ticks_(loss_ticks), profit_ticks_(profit_ticks),
          reissue_every_bar_(reissue_every_bar) {
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 1;
        set_syminfo_mintick(0.00001);
    }

    double live_qty() const { return position_qty_; }
    bool is_flat() const { return position_side_ == PositionSide::FLAT; }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("long", true, /*limit=*/1.17323, kNaN, 1.0,
                           "resting limit");
        }
        // quantbyboji shape: the ticks bracket is (re-)issued at global
        // scope on EVERY bar while the limit parent rests, so its
        // created_bar trails the parent's by the time the parent fills.
        if (bar_index_ == 0 || reissue_every_bar_) {
            strategy_exit("long", "long", kNaN, kNaN, kNaN, kNaN, kNaN,
                          100.0, "exit long", /*qty=*/1.0, "",
                          profit_ticks_, loss_ticks_);
        }
    }

private:
    double loss_ticks_;
    double profit_ticks_;
    bool reissue_every_bar_;
};

// The tape bracket: ta.atr(...)*mult/0.25 gave 0.00984 ticks for BOTH legs
// (a sub-tick offset). TV books the loss leg at 1.17322 — the level
// 1.17323 - 0.0000000984 lands on the tick below the fill — and the profit
// leg above the fill is never reached on the remaining path.
static constexpr double kTapeTicks = 0.00984241;

static std::vector<Bar> intrabar_parent_bars(double fill_bar_open,
                                             double fill_bar_low) {
    return {
        bar(1'000, 1.17367, 1.17395, 1.17348, 1.17356),   // signal bar
        bar(2'000, 1.17356, 1.17380, 1.17340, 1.17370),   // parent rests
        bar(3'000, 1.17370, 1.17390, 1.17345, 1.17360),   // parent rests
        bar(4'000, 1.17360, 1.17372, 1.17348, 1.17356),   // parent rests
        // The tape bar (2025-08-22 18:15Z): open above the 1.17323 limit,
        // the path reaches the low so the limit fills mid-path and the
        // loss leg is crossed on the remaining path of the SAME bar.
        bar(5'000, fill_bar_open, 1.17364, fill_bar_low, 1.17305),
        bar(6'000, 1.17307, 1.17326, 1.17249, 1.17260),
    };
}

static void check_intrabar_limit_parent_ticks(bool reissue_every_bar) {
    IntrabarLimitParentTicks probe(kTapeTicks, kTapeTicks, reissue_every_bar);
    std::vector<Bar> bars = intrabar_parent_bars(1.17356, 1.17288);
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.is_long);
    CHECK(t.entry_bar_index == 4);
    CHECK(t.exit_bar_index == 4);
    CHECK(near(t.entry_price, 1.17323, 1e-9));
    CHECK(near(t.exit_price, 1.17322, 1e-9));
    CHECK(near(t.qty, 1.0));
    CHECK(near(t.pnl, -0.00001, 1e-9));
    CHECK(t.exit_id == "long");
    CHECK(probe.is_flat());
}

// Control: parent fills AT the open (open <= limit) — the already-matching
// 140-trade population — the bracket walks the whole bar.
static void check_open_fill_limit_parent_ticks_control() {
    IntrabarLimitParentTicks probe(kTapeTicks, kTapeTicks, true);
    std::vector<Bar> bars = intrabar_parent_bars(1.17320, 1.17288);
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.entry_bar_index == 4);
    CHECK(t.exit_bar_index == 4);
    CHECK(near(t.entry_price, 1.17320, 1e-9));
    CHECK(near(t.exit_price, 1.17319, 1e-9));
}

// Control: neither leg is reached on the remaining path (the limit fills
// at the bar's low and the bar closes there) — the bracket rests into the
// next bar and gap-fills at its open.
static void check_intrabar_limit_parent_ticks_unreached_control() {
    IntrabarLimitParentTicks probe(kTapeTicks, kTapeTicks, true);
    std::vector<Bar> bars = intrabar_parent_bars(1.17356, 1.17323);
    bars[4].close = 1.17323;
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.last_error().empty());
    CHECK(probe.trade_count() == 1);
    if (probe.trade_count() != 1) return;
    const Trade& t = probe.get_trade(0);
    CHECK(t.entry_bar_index == 4);
    CHECK(t.exit_bar_index == 5);
    CHECK(near(t.entry_price, 1.17323, 1e-9));
    // Next bar opens at 1.17307, below the 1.17322 stop: gap fill.
    CHECK(near(t.exit_price, 1.17307, 1e-9));
}

int main() {
    std::printf("prearmed bracket legs resolve on the parent's fill bar\n");

    // (1a) dual-marketable bracket (bprakaash)
    check_dual_marketable_scratches_at_open(DualCell::ShortBothInside, false);
    check_dual_marketable_scratches_at_open(DualCell::ShortBothEqualOpen, false);
    check_dual_marketable_scratches_at_open(DualCell::LongBothInside, false);
    check_dual_marketable_scratches_at_open(DualCell::ShortBothInside, true);
    check_dual_marketable_scratches_at_open(DualCell::LongBothInside, true);
    check_explicit_qty_bracket_no_gap_control();

    // (1b) trail-carrying leg (stevenygabbyperez)
    check_trail_stop_gap(true, false, false);
    check_trail_stop_gap(false, false, false);
    check_trail_stop_gap(true, true, false);
    check_trail_stop_gap(true, true, true);
    check_trail_stop_intrabar_control();

    // (2) relative-ticks bracket of an intrabar limit parent (quantbyboji)
    check_intrabar_limit_parent_ticks(/*reissue_every_bar=*/true);
    check_intrabar_limit_parent_ticks(/*reissue_every_bar=*/false);
    check_open_fill_limit_parent_ticks_control();
    check_intrabar_limit_parent_ticks_unreached_control();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
