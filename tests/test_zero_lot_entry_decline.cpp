/*
 * test_zero_lot_entry_decline.cpp — an entry whose lot-floored opening
 * quantity is ZERO is declined cleanly (no fill, no trade row, no open trade)
 * instead of opening a phantom zero-quantity position.
 *
 * Ground truth: 3commas-3commas-bch-heikin-ashi-rsi-fade-short-strategy on
 * NASDAQ:AAPL 15m (qty_step 1 share, process_orders_on_close=true,
 * qty = 280 / close). TV tape: 2025-12-02 15:45 UTC close 286.96 ->
 * 280/286.96 = 0.9757 -> 0 shares -> NO row; the next TV entry is
 * 2025-12-09 18:15 UTC @ 278.35 qty 1 (280/278.38 = 1.0058 -> 1), and the
 * strategy keeps trading (26 TV entries through 2026-04-28). Pre-fix the
 * engine handed the floored 0 to open_fresh_position: strategy.position_size
 * read 0 (script believed it was flat, never placed its exit) while
 * strategy.opentrades read 1 and pyramiding=1 was saturated -> every later
 * entry dropped (2 engine trades vs 26).
 *
 * RED-1   explicit qty 0.9757 @ step 1, POOC     -> declined; NEXT >=1-share
 *         signal fills qty 1 (position SHORT 1, opentrades 1).
 * RED-2   CASH default sizing (qty=na, 280 cash) @ step 1 -> same.
 * RED-3   explicit qty 0.9757 @ step 1, next-bar-open fill (POOC=false).
 * RED-4   pyramiding DCA MARKET add usdt/close -> 0 @ step 1 -> declined, NO
 *         pyramiding slot consumed: the next >=1-share add still fills.
 * RED-5   same with a LIMIT add placed while in position (tv_carry_qty > 0 is
 *         snapshotted for it but is NOT a deferred-flip carry) -> declined.
 * GREEN-A same explicit qty @ step 0.0001 -> fills 0.9757 (unchanged path).
 * GREEN-B explicit qty 2.47 @ step 1 -> fills 2 (existing floor unchanged).
 * GREEN-C step 0 (corpus default), explicit qty 1 -> fills 1 (unchanged).
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

// Script chars (indexed by bar_index_):
//   'S' explicit SHORT market entry, qty = cash_ / close   (the 3commas shape)
//   'D' default-sized SHORT market entry (qty = na)
//   'F' explicit SHORT market entry, qty = fixed_qty_
//   'A' explicit SHORT market ADD,  qty = add_cash_ / close   (DCA safety order)
//   'L' explicit SHORT LIMIT add,   qty = add_cash_ / close, limit = limit_
//   'M' explicit SHORT LIMIT add,   qty = fixed_qty_,        limit = limit_
//   '.' nothing
class Probe : public BacktestEngine {
public:
    Probe(double qty_step, bool pooc, QtyType default_type, double default_value,
          int pyramiding = 1) {
        initial_capital_ = 10000.0;
        default_qty_type_ = default_type;
        default_qty_value_ = default_value;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.055;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        slippage_ = 3;
        syminfo_mintick_ = 0.01;
        qty_step_ = qty_step;
        pyramiding_ = pyramiding;
        process_orders_on_close_ = pooc;
    }
    std::string script;
    double cash_ = 280.0;
    double fixed_qty_ = 1.0;
    double add_cash_ = 280.0;
    double limit_ = kNaN;

    void on_bar(const Bar& bar) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'S': strategy_entry("E", false, kNaN, kNaN, cash_ / bar.close); break;
            case 'D': strategy_entry("E", false, kNaN, kNaN, kNaN); break;
            case 'F': strategy_entry("E", false, kNaN, kNaN, fixed_qty_); break;
            case 'A': strategy_entry("E", false, kNaN, kNaN, add_cash_ / bar.close); break;
            case 'L': strategy_entry("E", false, limit_, kNaN, add_cash_ / bar.close); break;
            case 'M': strategy_entry("E", false, limit_, kNaN, fixed_qty_); break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    using BacktestEngine::pyramid_entries_;
    double position_size() const { return signed_position_size(); }
    int opentrades() const { return (int)pyramid_entries_.size(); }
    using BacktestEngine::position_entry_count_;
    bool all_lots_positive() const {
        for (const auto& e : pyramid_entries_) if (!(e.qty > 0.0)) return false;
        return true;
    }
};

// The AAPL divergence bar (close 286.96, 280/close = 0.9757) followed by the
// bar TV actually entered on (close 278.38, 280/close = 1.0058).
static std::vector<Bar> aapl_bars() {
    return {
        mk_bar(1000, 285.00, 287.00, 284.50, 286.96),   // 'S' -> 0.9757 -> 0
        mk_bar(2000, 286.00, 286.50, 285.00, 286.04),   // '.' still flat in TV
        mk_bar(3000, 278.00, 279.00, 277.50, 278.38),   // 'S' -> 1.0058 -> 1
        mk_bar(4000, 278.00, 278.50, 277.00, 278.00),
    };
}

// RED-1. POOC explicit-qty short: 0.9757 -> 0 -> declined; next signal fills 1.
// Pre-fix: bar 0 opens a phantom SHORT with qty 0 (opentrades 1, size 0) and
// bar 2's entry is dropped by the pyramiding cap.
void test_red1_pooc_explicit_zero_lot_declined_then_next_fills() {
    std::printf("-- RED-1: POOC explicit zero-lot declined, next signal fills --\n");
    Probe eng(/*qty_step=*/1.0, /*pooc=*/true, QtyType::FIXED, 1.0);
    eng.script = "S.S.";
    std::vector<Bar> bars = aapl_bars();
    // Run all four bars and inspect end state + ledger: pre-fix the phantom
    // shows up as opentrades 1 with size 0 and a dropped second entry.
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -1.0, 1e-9);          // pre-fix: -0.0
    CHECK(eng.opentrades() == 1);
    CHECK_NEAR(eng.pyramid_entries_.back().qty, 1.0, 1e-9);  // pre-fix: 0
    // Fill at bar 2's close minus 3 ticks of slippage (short).
    CHECK_NEAR(eng.pyramid_entries_.back().price, 278.35, 1e-9);
    CHECK(eng.trade_count() == 0);                          // still open
}

// RED-2. CASH default sizing (qty = na, default 280 cash): frozen quantity
// floors to 0 at 286.96 -> declined (KI-72 covers only percent_of_equity);
// next signal fills 1.
void test_red2_cash_default_zero_lot_declined() {
    std::printf("-- RED-2: CASH default zero-lot declined --\n");
    Probe eng(/*qty_step=*/1.0, /*pooc=*/true, QtyType::CASH, 280.0);
    eng.script = "D.D.";
    std::vector<Bar> bars = aapl_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -1.0, 1e-9);
    CHECK(eng.opentrades() == 1);
    CHECK_NEAR(eng.pyramid_entries_.back().qty, 1.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// RED-3. Next-bar-open fill (POOC=false): the zero-lot order placed on bar 0
// is declined at bar 1's open; bar 2's order fills at bar 3's open.
void test_red3_next_bar_open_zero_lot_declined() {
    std::printf("-- RED-3: next-bar-open explicit zero-lot declined --\n");
    Probe eng(/*qty_step=*/1.0, /*pooc=*/false, QtyType::FIXED, 1.0);
    eng.script = "S.S.";
    std::vector<Bar> bars = aapl_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -1.0, 1e-9);
    CHECK(eng.opentrades() == 1);
    CHECK_NEAR(eng.pyramid_entries_.back().qty, 1.0, 1e-9);
    CHECK_NEAR(eng.pyramid_entries_.back().price, 278.00 - 0.03, 1e-9);
}

// GREEN-A. Same explicit qty at step 0.0001 (ETH lane): fills 0.9757 on bar 0
// exactly as before; bar 2's entry is then capped by pyramiding=1.
void test_greenA_fine_step_unchanged() {
    std::printf("-- GREEN-A: step 0.0001 fills 0.9757 (unchanged) --\n");
    Probe eng(/*qty_step=*/0.0001, /*pooc=*/true, QtyType::FIXED, 1.0);
    eng.script = "S.S.";
    std::vector<Bar> bars = aapl_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -0.9757, 1e-9);
    CHECK(eng.opentrades() == 1);
    CHECK_NEAR(eng.pyramid_entries_.back().price, 286.93, 1e-9);
}

// GREEN-B. Explicit 2.47 at step 1 floors to 2 (existing behavior).
void test_greenB_floor_above_one_unchanged() {
    std::printf("-- GREEN-B: 2.47 @ step 1 -> 2 (unchanged) --\n");
    Probe eng(/*qty_step=*/1.0, /*pooc=*/true, QtyType::FIXED, 1.0);
    eng.fixed_qty_ = 2.47;
    eng.script = "F...";
    std::vector<Bar> bars = aapl_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -2.0, 1e-9);
}

// GREEN-C. qty_step 0 (corpus default), explicit qty 1 -> fills 1.
void test_greenC_step_zero_unchanged() {
    std::printf("-- GREEN-C: step 0 explicit qty 1 (unchanged) --\n");
    Probe eng(/*qty_step=*/0.0, /*pooc=*/true, QtyType::FIXED, 1.0);
    eng.fixed_qty_ = 1.0;
    eng.script = "F...";
    std::vector<Bar> bars = aapl_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -1.0, 1e-9);
}

// RED-4. 3commas DCA shape (pyramiding=2, POOC, same id "E"): base 2 shares,
// then a safety-order MARKET add sized 280/close at close 290 -> 0.9655 -> 0.
// TV does not place it: no lot, no trade row, NO pyramiding slot spent, so the
// next add (qty 1) still fills -> position -3, two positive lots.
// Pre-fix: the zero add opens a qty-0 lot (count 2 == cap) and the 1-share add
// is dropped by the cap -> position -2 with a qty-0 phantom lot.
void test_red4_market_add_zero_lot_declined_no_slot() {
    std::printf("-- RED-4: DCA MARKET zero-lot add declined, slot preserved --\n");
    Probe eng(/*qty_step=*/1.0, /*pooc=*/true, QtyType::FIXED, 1.0, /*pyramiding=*/2);
    eng.fixed_qty_ = 2.0;
    eng.script = "FAF.";
    std::vector<Bar> bars = {
        mk_bar(1000, 250.0, 251.0, 249.0, 250.0),   // F: base 2 @ 249.97
        mk_bar(2000, 289.0, 291.0, 288.0, 290.0),   // A: 280/290 = 0.9655 -> 0 -> declined
        mk_bar(3000, 292.0, 293.0, 291.0, 292.0),   // F: add 2 -> fills (slot free)
        mk_bar(4000, 292.0, 292.5, 291.5, 292.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -4.0, 1e-9);          // pre-fix: -2.0
    CHECK(eng.opentrades() == 2);
    CHECK(eng.position_entry_count_ == 2);
    CHECK(eng.all_lots_positive());                       // pre-fix: qty-0 lot present
    CHECK_NEAR(eng.pyramid_entries_.back().qty, 2.0, 1e-9);
    CHECK_NEAR(eng.pyramid_entries_.back().price, 292.0 - 0.03, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// RED-5. Same DCA shape but the safety order is a LIMIT add placed while
// already short (POOC=false). strategy_entry snapshots the live position into
// tv_carry_qty for every priced entry, yet the add kernel never applies that
// carry (it is a deferred-flip rule for entries firing from FLAT on the
// opposite side) — so it must not exempt the zero-lot add. Pre-fix
// (1f2681d): carry 2 + own 0 > eps -> admitted -> qty-0 lot, slot burned, the
// later 1-share limit add is rejected at placement by the pyramiding cap.
void test_red5_limit_add_zero_lot_declined_no_slot() {
    std::printf("-- RED-5: DCA LIMIT zero-lot add declined, slot preserved --\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 250.0, 251.0, 249.0, 250.0),   // F: base 2, fills bar1 open
        mk_bar(2000, 250.0, 251.0, 249.0, 250.0),   // L: limit 290, qty 280/290 -> 0
        mk_bar(3000, 290.0, 291.0, 289.0, 290.0),   // limit touched: zero-lot add -> declined
        mk_bar(4000, 290.0, 291.0, 289.0, 290.0),   // bar 3: limit 292 qty 1 (placement OK: count 1)
        mk_bar(5000, 292.0, 293.0, 291.0, 292.0),   // fills 1 @ 292 -> position -3
        mk_bar(6000, 292.0, 292.5, 291.5, 292.0),
    };
    // Bar 3 places the 1-share limit add directly (no script char for it).
    struct Driver : Probe {
        using Probe::Probe;
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 3) {
                strategy_entry("E", false, /*limit=*/292.0, kNaN, /*qty=*/1.0);
                return;
            }
            Probe::on_bar(bar);
        }
    };
    Driver d(/*qty_step=*/1.0, /*pooc=*/false, QtyType::FIXED, 1.0, /*pyramiding=*/2);
    // Size the limit add from the LIMIT price the way a DCA script does
    // (usdt / trigger price): 280 / 290 = 0.9655 -> 0 shares. 'L' divides
    // add_cash_ by the bar close (250), so scale it to land on 280/290.
    d.add_cash_ = 280.0 * 250.0 / 290.0;
    d.limit_ = 290.0;
    d.fixed_qty_ = 2.0;
    d.script = "FL....";
    d.run(bars.data(), (int)bars.size());
    CHECK(d.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(d.position_size(), -3.0, 1e-9);            // pre-fix: -2.0
    CHECK(d.opentrades() == 2);                           // base + 1-share add
    CHECK(d.position_entry_count_ == 2);
    CHECK(d.all_lots_positive());                         // pre-fix: qty-0 lot
    CHECK_NEAR(d.pyramid_entries_.back().qty, 1.0, 1e-9);
    CHECK_NEAR(d.pyramid_entries_.back().price, 292.0, 1e-9);   // limit fills unslipped
    CHECK(d.trade_count() == 0);
}

// GREEN-D. A LIMIT add that survives the floor (qty 1.12 -> 1) fills as before
// and spends its slot normally.
void test_greenD_limit_add_one_share_unchanged() {
    std::printf("-- GREEN-D: LIMIT add 1.12 -> 1 fills (unchanged) --\n");
    Probe eng(/*qty_step=*/1.0, /*pooc=*/false, QtyType::FIXED, 1.0, /*pyramiding=*/2);
    eng.fixed_qty_ = 2.0;
    eng.add_cash_ = 280.0;                                 // 280/250 = 1.12 -> 1
    eng.limit_ = 290.0;
    eng.script = "FL....";
    std::vector<Bar> bars = {
        mk_bar(1000, 250.0, 251.0, 249.0, 250.0),
        mk_bar(2000, 250.0, 251.0, 249.0, 250.0),
        mk_bar(3000, 290.0, 291.0, 289.0, 290.0),
        mk_bar(4000, 290.0, 291.0, 289.0, 290.0),
        mk_bar(5000, 292.0, 293.0, 291.0, 292.0),
        mk_bar(6000, 292.0, 292.5, 291.5, 292.0),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_size(), -3.0, 1e-9);
    CHECK(eng.opentrades() == 2);
    CHECK(eng.position_entry_count_ == 2);
    CHECK_NEAR(eng.pyramid_entries_.back().qty, 1.0, 1e-9);
    CHECK_NEAR(eng.pyramid_entries_.back().price, 290.0, 1e-9);
}

}  // namespace

int main() {
    std::printf("--- zero_lot_entry_decline ---\n");
    test_red1_pooc_explicit_zero_lot_declined_then_next_fills();
    test_red2_cash_default_zero_lot_declined();
    test_red3_next_bar_open_zero_lot_declined();
    test_red4_market_add_zero_lot_declined_no_slot();
    test_red5_limit_add_zero_lot_declined_no_slot();
    test_greenD_limit_add_one_share_unchanged();
    test_greenA_fine_step_unchanged();
    test_greenB_floor_above_one_unchanged();
    test_greenC_step_zero_unchanged();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
