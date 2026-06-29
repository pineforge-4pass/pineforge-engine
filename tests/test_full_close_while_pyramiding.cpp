/*
 * test_full_close_while_pyramiding.cpp — regression for a full-position
 * take-profit exit (strategy.exit with qty_percent=100, no explicit qty)
 * RE-ISSUED every bar while the position is still GROWING via pyramiding/DCA.
 *
 * Bug (pre-fix): compute_exit_reserved_qty honoured the preserved (frozen)
 * reserved qty captured by clear_existing_exit_order on re-issue even for a
 * 100% exit. So the reserved qty stayed pinned at the size from the bar the
 * exit was first placed, instead of re-expanding to 100% of the now-larger
 * position. At the TP touch the engine closed only the first FIFO lot at the
 * true TP price; the residual lots exited one bar late at a re-priced limit /
 * next-bar-open. One logical exit fragmented across two bars → wrong exit
 * prices + inflated trade count + inflated PnL.
 *
 * Fix: gate the preserved-qty carry to genuine PARTIAL re-issues only
 * (qp < 100 - kFullPercentEps). A re-issued full exit falls through to
 * recompute requested = position_qty * qp/100 = full grown position; since
 * clear_existing_exit_order already removed the prior order, available == full
 * current position → reserves 100% → closes the whole stack at the single TP
 * touch.
 *
 * Minimal repro (matches the TradingView-verified expectation):
 *   pyramiding=5; three entries qty=10 @100 (avg 100); every bar
 *   strategy.exit("TP", limit=avg*1.04) (TP=104); trigger bar
 *   O=101 H=110 L=100 C=108.
 *   EXPECTED (TV): all 30 units close @104 on the trigger bar, PnL=120.
 *   BUG:           lot#1 @104, lots#2-3 one bar late @108, PnL=200.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

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

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// ─────────────────────────────────────────────────────────────────────
// A full-close TP re-issued every bar while the position grows via
// pyramiding must re-expand to 100% of the CURRENT (grown) position and
// close the entire stack at the single TP touch — not freeze at the size
// from the bar it was first placed.
//
// pyramiding=5, FIXED qty=10. Three market entries issued on bars 0,1,2
// fill at bars 1,2,3 open=100 → position 30 @ avg 100. The TP exit
// (qty_percent=100, limit = avg*1.04 = 104) is re-issued on EVERY bar the
// position is long, so it is repeatedly cleared+rebuilt as the stack grows.
// Trigger bar (index 4): O=101 H=110 L=100 C=108 → 104 is in [100,110] and
// the up-leg touches it.
//
// The engine records one closed Trade per entry lot (TV pyramiding
// semantics), so the 30-unit stack closes as THREE 10-unit trades — but the
// fix requires every lot to exit at the single TP touch (price 104, on bar
// 4). Pre-fix the reserved qty froze at 10 (the size from the bar the exit
// was first placed), so only the first FIFO lot closed at 104 on bar 4 and
// the residual two lots exited one bar LATE at the re-priced limit / next-bar
// open (108 on bar 5) — fragmenting one logical exit across two bars and
// inflating realised PnL to 40 + 80 + 80 = 200. With the fix all three lots
// close at 104 on bar 4: PnL = 30 * (104 - 100) = 120.
// ─────────────────────────────────────────────────────────────────────
static void test_full_close_reexpands_while_pyramiding() {
    std::printf("test_full_close_reexpands_while_pyramiding\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 10.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 5;
            syminfo_mintick_ = 0.01;
        }
        // Expose the protected position-size accessor for external assertions.
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            // Three DCA entries on bars 0,1,2 → fill @100 on bars 1,2,3.
            if (bar_index_ <= 2) {
                strategy_entry("L", true, kNaN, kNaN, kNaN, "enter");
            }
            // Re-issue the FULL-CLOSE TP on every bar the position is long,
            // pricing it off the live average. This is the path that froze
            // the reserved qty pre-fix.
            if (position_side_ == PositionSide::LONG) {
                double tp = position_entry_price_ * 1.04;
                strategy_exit("TP", "L", /*limit=*/tp, /*stop=*/kNaN,
                              kNaN, kNaN, kNaN, /*qty_percent=*/100.0, "", kNaN, "");
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100, 600'000),    // bar0: place entry #1
        mk(100, 100, 100, 100, 1'200'000),  // bar1: entry #1 fills @100; place #2; arm TP
        mk(100, 100, 100, 100, 1'800'000),  // bar2: entry #2 fills @100; place #3
        mk(100, 100, 100, 100, 2'400'000),  // bar3: entry #3 fills @100 → 30 @ avg 100
        mk(101, 110, 100, 108, 3'000'000),  // bar4: TP@104 touched on up-leg
        mk(108, 109, 107, 108, 3'600'000),  // bar5: settle
    };
    p.run(bars, 6);

    // Three entry lots → three closed trades.
    CHECK(p.trade_count() == 3);
    if (p.trade_count() != 3) return;

    // The load-bearing assertion: EVERY lot exits at the single TP touch —
    // price 104, on the trigger bar (index 4). Pre-fix, lots #2 and #3 exited
    // one bar late (bar 5) at 108.
    double pnl = 0.0;
    double total_qty = 0.0;
    for (int i = 0; i < 3; ++i) {
        const Trade& t = p.get_trade(i);
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 104.0));   // not 108 (the bug's late re-price)
        CHECK(t.exit_bar_index == 4);       // not bar 5 (one bar late)
        CHECK(near(t.qty, 10.0));
        pnl += t.pnl;
        total_qty += t.qty;
    }

    // Whole 30-unit stack closed.
    CHECK(near(total_qty, 30.0));

    // Realised PnL = 30 * (104 - 100) = 120 (NOT 200, which was the
    // fragmented two-bar exit's inflated result).
    CHECK(near(pnl, 120.0, 1e-4));

    // Position fully flat after the exit.
    CHECK(near(p.pos_size(), 0.0));
}

// ─────────────────────────────────────────────────────────────────────
// Guard: a genuine PARTIAL (qty_percent < 100) re-issue must STILL honour
// the preserved reserved qty so it does not double-reserve against the same
// from_entry as the position grows. Here a 50% exit is issued once (on the
// first long bar, capturing 50% of 10 = 5), then re-issued every bar; the
// preserved 5 must be carried even though the position later grows to 30,
// so the partial closes 5 units at its limit — not 50% of the grown stack.
// ─────────────────────────────────────────────────────────────────────
static void test_partial_reissue_keeps_preserved_qty() {
    std::printf("test_partial_reissue_keeps_preserved_qty\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 10.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 5;
            syminfo_mintick_ = 0.01;
        }
        // Expose the protected position-size accessor for external assertions.
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ <= 2) {
                strategy_entry("L", true, kNaN, kNaN, kNaN, "enter");
            }
            if (position_side_ == PositionSide::LONG) {
                // 50% partial, priced out of range until the trigger bar so it
                // is repeatedly cleared+rebuilt (exercising the preserved-qty
                // carry) without firing early.
                strategy_exit("TP", "L", /*limit=*/106.0, /*stop=*/kNaN,
                              kNaN, kNaN, kNaN, /*qty_percent=*/50.0, "", kNaN, "");
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100, 600'000),
        mk(100, 100, 100, 100, 1'200'000),  // entry #1 fills; partial armed @ 50% of 10 = 5
        mk(100, 100, 100, 100, 1'800'000),  // entry #2 fills
        mk(100, 100, 100, 100, 2'400'000),  // entry #3 fills → 30 @ avg 100
        mk(101, 110, 100, 108, 3'000'000),  // bar4: 106 touched
        mk(108, 109, 107, 108, 3'600'000),
    };
    p.run(bars, 6);

    // The partial preserved its original reserved qty (5), not 50% of the
    // grown 30 (=15). One partial trade closes 5 at 106; position stays open.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() < 1) return;
    CHECK(near(p.get_trade(0).exit_price, 106.0));
    CHECK(near(p.get_trade(0).qty, 5.0));
    // 30 - 5 = 25 remain open.
    CHECK(near(p.pos_size(), 25.0));
}

int main() {
    test_full_close_reexpands_while_pyramiding();
    test_partial_reissue_keeps_preserved_qty();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
