/*
 * test_default_qty_signal_freeze.cpp — TradingView freezes DEFAULT (qty=na)
 * percent_of_equity / cash market-order sizing at the SIGNAL bar's close;
 * the market order fills at the next bar's open carrying the frozen qty.
 *
 * Pins (see frozen_default_market_qty in engine.hpp for the rule):
 *   A. Reversal with close(S) != open(S+1): the new lot's qty equals
 *      equity_S / close(S) where equity_S = capital + realized + open mark at
 *      close(S) — computed with the OLD position still open. The pre-freeze
 *      fill-time evaluation was wrong three ways at once (double-counted the
 *      just-closed lot's PnL, marked open profit at the FILL bar's close, and
 *      divided by the fill price); a frozen qty of exactly equity_S/close(S)
 *      excludes all three.
 *   B. Flat entry with a close→open gap DOWN: qty = equity_S / close(S), not
 *      equity / open(S+1) — pins the divisor with no position in play.
 *   C. Flat all-in (pct=100) zero-commission entry with a close→open gap UP
 *      whose frozen-qty notional exceeds the sizing equity by more than one
 *      lot is REJECTED at fill (design-cntvxiao-gap-reject): the frozen qty is
 *      never re-sized, and the whole true-flat entry is silently dropped. The
 *      freeze itself still never RE-SIZES on a gap — pin B (gap DOWN) and the
 *      affordable cases keep filling the frozen equity_S / close(S) lot.
 *   D. process_orders_on_close=true: signal bar == fill bar and fill price ==
 *      close(S), so the frozen qty is identical to the legacy fill-time
 *      computation — POC sizing is unchanged.
 *   E. CASH default sizing freezes at close(S) too: qty = cash / close(S),
 *      not cash / open(S+1).
 *   H. An ordinary true-flat omitted-qty MARKET dispatch preserves the
 *      already-quantized frozen quantity.  It must not apply qty_step a
 *      second time and lose one lot at an exact binary boundary.
 *   I. The adjacent explicit-qty shape still receives its ordinary single
 *      qty_step application; the frozen-quantity exception does not widen.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
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

#define CHECK_NEAR(a, b, tol)                                                  \
    do {                                                                       \
        double _a = (a), _b = (b);                                             \
        if (!(std::fabs(_a - _b) <= (tol))) {                                  \
            std::printf("  FAIL  %s:%d  %s == %.10f, expected %.10f\n",        \
                        __FILE__, __LINE__, #a, _a, _b);                       \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Scripted probe: runs a fixed action per bar_index. All prices in the tests
// are on-tick (mintick 0.01) so the zero-slippage directional snap is an
// identity and fills land exactly at the bar prices.
class Probe : public BacktestEngine {
public:
    Probe(QtyType qty_type, double qty_value, bool poc) {
        initial_capital_ = 10000.0;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_value_ = 0.0;
        process_orders_on_close_ = poc;
        // The all-in (100%) probes hold fully-leveraged positions whose
        // liquidation price sits at the entry; disable forced liquidation so
        // the sizing freeze is the only mechanism under test.
        margin_call_enabled_ = false;
    }
    // action per bar: 'L' = default-sized long entry, 'S' = default-sized
    // short entry, 'C' = close all, '.' = nothing.
    std::string script;
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("L", true); break;
            case 'S': strategy_entry("S", false); break;
            case 'C': strategy_close_all(); break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    const std::vector<Trade>& all_trades() const { return trades_; }
};

// A. Percent-of-equity reversal, close(S) != open(S+1).
//
//   bar0  100/100/100/100   on_bar: long entry (frozen: 10000/100 = 100)
//   bar1  100/112/ 99/110   long fills @open 100 qty 100
//                           on_bar: short entry — SIGNAL bar. equity_S =
//                           10000 + (110-100)*100 = 11000 (long still OPEN,
//                           marked at close(S)=110); frozen qty =
//                           11000/110 = 100 exactly.
//   bar2  108/109/ 99/101   reversal fills @open 108: long closes (+800),
//                           short opens with the FROZEN qty 100.
//                           Pre-freeze fill-time sizing would have produced
//                           (10800 + 100)/108 = 100.9259... — the realized
//                           +800 double-counted via the stale open-profit
//                           mark at the FILL bar's close (101), divided by
//                           the fill price: all three defects at once.
//   bar3  101/101/101/101   on_bar: close_all
//   bar4  101/101/101/101   short closes @open 101 (+700)
void test_reversal_freeze() {
    std::printf("-- A: percent_of_equity reversal freeze --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, /*poc=*/false);
    eng.script = "LS.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 112, 99, 110),
        mk_bar(3000, 108, 109, 99, 101),
        mk_bar(4000, 101, 101, 101, 101),
        mk_bar(5000, 101, 101, 101, 101),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 2);
    if (eng.trade_count() == 2) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK(t0.is_long);
        CHECK_NEAR(t0.entry_price, 100.0, 1e-9);
        CHECK_NEAR(t0.qty, 100.0, 1e-9);
        CHECK_NEAR(t0.exit_price, 108.0, 1e-9);
        CHECK_NEAR(t0.pnl, 800.0, 1e-9);
        const Trade& t1 = eng.all_trades()[1];
        CHECK(!t1.is_long);
        CHECK_NEAR(t1.entry_price, 108.0, 1e-9);
        // THE pin: frozen at the signal bar (11000/110), NOT the fill-time
        // double-count (100.9259...).
        CHECK_NEAR(t1.qty, 100.0, 1e-9);
        CHECK_NEAR(t1.exit_price, 101.0, 1e-9);
        CHECK_NEAR(t1.pnl, 700.0, 1e-9);
    }
}

// B. Flat entry, gap DOWN: divisor is close(S), not the fill price.
//   bar0  100/100/100/100   on_bar: long entry — frozen 10000/100 = 100
//   bar1   98/ 98/ 98/ 98   fills @98: qty must stay 100 (legacy fill-time
//                           sizing would give 10000/98 = 102.04...).
//                           Admission: 100*98 = 9800 <= 10000 -> admitted.
void test_flat_gap_down_divisor() {
    std::printf("-- B: flat entry, divisor = close(S) --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, /*poc=*/false);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 98, 98, 98, 98),
        mk_bar(3000, 98, 98, 98, 98),
        mk_bar(4000, 98, 98, 98, 98),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK_NEAR(t0.entry_price, 98.0, 1e-9);
        CHECK_NEAR(t0.qty, 100.0, 1e-9);
    }
}

// C. Flat all-in (pct=100) zero-comm entry, gap UP: REJECTED at fill. The
//    frozen qty 10000/100 = 100 stays frozen (no re-size), but the fill
//    notional 100*102 = 10200 exceeds the 10000 sizing equity by more than
//    one lot (qty_step 0), so TV silently drops the entry
//    (design-cntvxiao-gap-reject) and the later close_all has nothing to
//    close. (Pre-gap-reject this filled 100 @ 102.)
//   bar0  100/100/100/100   on_bar: long entry — frozen qty 10000/100 = 100
//   bar1  102/103/101/102   fill 102 -> 100*102 = 10200 > 10000 -> DROP
void test_flat_gap_up_rejected() {
    std::printf("-- C: flat all-in zero-comm gap-up rejected --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, /*poc=*/false);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 102, 103, 101, 102),
        mk_bar(3000, 102, 102, 102, 102),
        mk_bar(4000, 102, 102, 102, 102),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);   // was 1 (filled 100@102); close_all inert
    CHECK(eng.position_side_ == PositionSide::FLAT);
}

// D. process_orders_on_close=true: unchanged. Signal bar == fill bar, fill
//    price == close(S) — frozen and legacy sizing coincide.
//   bar0  100/100/100/100   on_bar: long entry; fills same bar @close 100
//                           qty = 10000/100 = 100 (as before the freeze)
//   bar1  105/105/105/105   on_bar: close_all; fills same bar @close 105
void test_poc_unchanged() {
    std::printf("-- D: process_orders_on_close unchanged --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, /*poc=*/true);
    eng.script = "LC";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 105, 105, 105, 105),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK_NEAR(t0.entry_price, 100.0, 1e-9);
        CHECK_NEAR(t0.qty, 100.0, 1e-9);
        CHECK_NEAR(t0.exit_price, 105.0, 1e-9);
        CHECK_NEAR(t0.pnl, 500.0, 1e-9);
    }
}

// E. CASH default sizing freezes at close(S) too.
//   bar0  100/100/100/100   on_bar: long entry — frozen 1000/100 = 10
//   bar1   98/...           fills @98: qty 10, not 1000/98 = 10.204...
void test_cash_freeze() {
    std::printf("-- E: cash default sizing freeze --\n");
    Probe eng(QtyType::CASH, 1000.0, /*poc=*/false);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 98, 98, 98, 98),
        mk_bar(3000, 98, 98, 98, 98),
        mk_bar(4000, 98, 98, 98, 98),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK_NEAR(t0.entry_price, 98.0, 1e-9);
        CHECK_NEAR(t0.qty, 10.0, 1e-9);
    }
}

// F. isnan(order.qty) semantics survive the freeze — OCA reduce.
//    reduce_oca_group cancels a DEFAULT-sized sibling outright on any group
//    fill (engine_orders.cpp: "default-sized: cancel"). If the freeze wrote
//    the frozen quantity into order.qty, the sibling would instead take
//    ``qty -= filled_qty`` and SURVIVE — here B (frozen 100) would live on
//    as 95 after A's 5-lot close leg and open a phantom 95-lot short.
//   bar0  100  on_bar: explicit long qty=5 ("L")
//   bar1  100  L fills @100 (LONG 5); on_bar: two default-sized RAW shorts
//              A + B in OCA group "G" (strategy.oca.reduce), frozen qty 100
//   bar2  100  A fills first: opposite raw fill closes the LONG (5 lots,
//              filled_qty=5) -> reduce_oca_group must CANCEL default-sized B
//   end        position FLAT, exactly 1 trade (the closed long)
class OcaProbe : public BacktestEngine {
public:
    OcaProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_call_enabled_ = false;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 5.0);
        } else if (bar_index_ == 1) {
            strategy_order("A", false, kNaN, kNaN, kNaN, "G", /*oca_type=*/2);
            strategy_order("B", false, kNaN, kNaN, kNaN, "G", /*oca_type=*/2);
        }
    }
    using BacktestEngine::position_side_;
    const std::vector<Trade>& all_trades() const { return trades_; }
};

void test_oca_default_sibling_cancelled() {
    std::printf("-- F: default-sized OCA sibling still cancelled --\n");
    OcaProbe eng;
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),
        mk_bar(3000, 100, 100, 100, 100),
        mk_bar(4000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() >= 1) {
        CHECK_NEAR(eng.all_trades()[0].qty, 5.0, 1e-9);
    }
}

// G. isnan(order.qty) semantics survive the freeze — reversal-bracket
//    binding. strategy_exit defers its reservation (qty=NaN -> full exit of
//    the eventual lot) when its from_entry is a PENDING DEFAULT-sized entry
//    OPPOSITE the live position (engine_strategy_commands.cpp,
//    bind_to_pending_reversal_entry). If the freeze wrote into order.qty the
//    binding test would see an explicit qty, freeze the bracket at the OLD
//    position's size (1), and strand a 99-lot dust short when it fires.
//   bar0  100            on_bar: explicit long qty=1 ("L")
//   bar1  100            L fills @100 (LONG 1); on_bar: default-sized short
//                        "S" (frozen 10000/100 = 100) + bracket
//                        strategy.exit("SX", from_entry="S", stop=105)
//   bar2  100            S fills @100: flip -> close LONG 1, open SHORT 100
//   bar3  100/106/100    SX buy-stop fires @105 -> must close the FULL 100
//   end                  position FLAT; short trade qty 100, pnl -500
class ReversalBindProbe : public BacktestEngine {
public:
    ReversalBindProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_call_enabled_ = false;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 1.0);
        } else if (bar_index_ == 1) {
            strategy_entry("S", false);
            strategy_exit("SX", "S", kNaN, /*stop_price=*/105.0);
        }
    }
    using BacktestEngine::position_side_;
    const std::vector<Trade>& all_trades() const { return trades_; }
};

void test_reversal_bracket_binding_survives_freeze() {
    std::printf("-- G: default-sized reversal-bracket binding survives --\n");
    ReversalBindProbe eng;
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),
        mk_bar(3000, 100, 100, 100, 100),
        mk_bar(4000, 100, 106, 100, 100),
        mk_bar(5000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 2);
    if (eng.trade_count() == 2) {
        const Trade& t1 = eng.all_trades()[1];
        CHECK(!t1.is_long);
        CHECK_NEAR(t1.qty, 100.0, 1e-9);   // full frozen lot, no 99-lot dust
        CHECK_NEAR(t1.entry_price, 100.0, 1e-9);
        CHECK_NEAR(t1.exit_price, 105.0, 1e-9);
        CHECK_NEAR(t1.pnl, -500.0, 1e-9);
    }
}

// H/I. Binary-boundary dispatch regression for the ordinary true-flat
// omitted-qty MARKET path.
//
// The raw placement quantity is 6279.0000001 / 1000 = 6.2790000001.  Its
// first qty_step floor is 6.2790.  In binary64, that result divided by 0.0001
// is 62789.999999..., so applying the same floor a SECOND time produces
// 6.2789.  frozen_default_qty is already exchange-quantized at placement;
// dispatch must preserve it in the position, physical lot, and logical id
// ledger.  Before the C fix, apply_market_order_fill handed the frozen value
// to enter_market_from_flat as an ordinary FIXED quantity and it was floored
// again.
class FrozenDispatchBoundaryProbe : public BacktestEngine {
public:
    explicit FrozenDispatchBoundaryProbe(bool explicit_qty)
        : explicit_qty_(explicit_qty) {
        initial_capital_ = 6279.0000001;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        qty_step_ = 0.0001;
        margin_call_enabled_ = false;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        if (explicit_qty_) {
            // Adjacent control: an explicit raw quantity is not frozen and
            // must still receive exactly one ordinary qty_step floor.
            strategy_entry("BOUNDARY", true, kNaN, kNaN, 6.2790000001);
        } else {
            strategy_entry("BOUNDARY", true);
        }
    }

    double one_floor_qty() const { return apply_qty_step(6.2790000001); }
    double two_floor_qty() const { return apply_qty_step(one_floor_qty()); }
    PositionSide position_side() const { return position_side_; }
    double position_qty() const { return position_qty_; }
    int live_lot_count() const { return static_cast<int>(pyramid_entries_.size()); }
    double live_lot_qty() const {
        return pyramid_entries_.empty() ? 0.0 : pyramid_entries_.front().qty;
    }
    std::string live_lot_id() const {
        return pyramid_entries_.empty() ? "" : pyramid_entries_.front().entry_id;
    }
    double id_ledger_qty(const std::string& id) const {
        const auto it = id_unclosed_qty_.find(id);
        return it == id_unclosed_qty_.end() ? 0.0 : it->second;
    }

private:
    bool explicit_qty_;
};

static std::vector<Bar> frozen_dispatch_boundary_bars() {
    return {
        mk_bar(1000, 1000.0, 1000.0, 1000.0, 1000.0),
        mk_bar(2000, 1000.0, 1000.0, 1000.0, 1000.0),
    };
}

void test_frozen_true_flat_market_dispatch_is_not_refloored() {
    std::printf("-- H: frozen true-flat MARKET dispatch is not re-floored --\n");
    FrozenDispatchBoundaryProbe eng(/*explicit_qty=*/false);
    auto bars = frozen_dispatch_boundary_bars();
    eng.run(bars.data(), static_cast<int>(bars.size()));

    // Pin the test's binary boundary independently of the dispatch result.
    CHECK_NEAR(eng.one_floor_qty(), 6.2790, 1e-12);
    CHECK_NEAR(eng.two_floor_qty(), 6.2789, 1e-12);

    CHECK(eng.position_side() == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty(), 6.2790, 1e-12);
    CHECK(eng.live_lot_count() == 1);
    CHECK_NEAR(eng.live_lot_qty(), 6.2790, 1e-12);
    CHECK(eng.live_lot_id() == "BOUNDARY");
    CHECK_NEAR(eng.id_ledger_qty("BOUNDARY"), 6.2790, 1e-12);
}

void test_explicit_true_flat_market_keeps_single_floor() {
    std::printf("-- I: explicit true-flat MARKET keeps one qty floor --\n");
    FrozenDispatchBoundaryProbe eng(/*explicit_qty=*/true);
    auto bars = frozen_dispatch_boundary_bars();
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.position_side() == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty(), 6.2790, 1e-12);
    CHECK(eng.live_lot_count() == 1);
    CHECK_NEAR(eng.live_lot_qty(), 6.2790, 1e-12);
    CHECK(eng.live_lot_id() == "BOUNDARY");
    CHECK_NEAR(eng.id_ledger_qty("BOUNDARY"), 6.2790, 1e-12);
}

}  // namespace

int main() {
    std::printf("--- default_qty_signal_freeze ---\n");
    test_reversal_freeze();
    test_flat_gap_down_divisor();
    test_flat_gap_up_rejected();
    test_poc_unchanged();
    test_cash_freeze();
    test_oca_default_sibling_cancelled();
    test_reversal_bracket_binding_survives_freeze();
    test_frozen_true_flat_market_dispatch_is_not_refloored();
    test_explicit_true_flat_market_keeps_single_floor();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
