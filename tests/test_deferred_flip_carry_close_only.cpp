/*
 * test_deferred_flip_carry_close_only.cpp — regression for a deferred-flip
 * carry priced ENTRY that flips the opposite position but must NOT open its
 * own leg.
 *
 * Bug (pre-fix): a pending stop/limit ENTRY that reaches its trigger while an
 * OPPOSITE position is live performs a FULL reversal (close the opposite,
 * open the new direction) whenever ``created_position_side != FLAT``. The
 * close-only bracket path (apply_entry_order_fill's ``close_only_opposite``)
 * only fired for ``created_position_side == FLAT``. So a stop armed during a
 * PRIOR position cycle — a same-id "S" stop placed while SHORT, that survives
 * a flip to LONG and then triggers against that LONG — reopened a fresh SHORT
 * at the (stale) stop level instead of just closing the long. TradingView
 * closes the long and re-arms the entry (its open leg is superseded by the
 * same-bar re-issue); the ungated engine emitted a phantom short.
 * On corpus/validation/pyramid-deferred-flip-close-all-01 this was 25 phantom
 * / one-bar-early shorts (countAbsDelta 22 → 2).
 *
 * Fix: the close_only_opposite gate is ``created_position_side != position_side_``
 * (a reduce-only flip whenever the order was NOT placed in the cycle of the
 * position it now reverses), and the created!=FLAT case routes through
 * ``flip_market_position_to(..., close_only=true)`` which closes the whole
 * opposite position and stays flat.
 *
 * A genuine SAME-cycle reverse (the stop was placed while already holding the
 * position it flips: created_position_side == the reversed side) must STILL
 * open the new leg — the second test guards that.
 *
 * Exemplar in the wild (covered by the corpus run, reproduced minimally here):
 * pyramid-deferred-flip-close-all-01, the 2025-04-13 19:30 UTC phantom short —
 * TV closes the long at the stale 1589.71 stop ("flip short stop") and opens
 * NO short; the pre-fix engine opened a phantom short there.
 *
 * KNOWN APPROXIMATION (out of scope, no ground truth): the gate
 * created_position_side != position_side_ approximates "the order predates
 * this position instance". A double flip — created LONG, the position flips
 * SHORT, then flips LONG again while the order is still pending — is
 * misclassified as same-cycle (created LONG == current LONG) and would open.
 * No export pins this case; left for a future rule-first cycle.
 */

#include <cmath>
#include <cstdio>
#include <limits>

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
// Deferred-flip carry: a short stop "S" is armed while SHORT (so its
// created_position_side is SHORT), the position then flips to LONG via a
// market entry, and "S" survives and triggers against that LONG. TV closes
// the long only; the engine must NOT open a phantom short.
//
//   bar0: place market short "SH"
//   bar1: SH fills @100 → SHORT 1; arm "S" short stop @95 (created SHORT)
//   bar2: place market long "L"
//   bar3: L fills @100 → reverses to LONG 1 (SH closed @100). "S"@95 pending,
//         still carrying created_position_side = SHORT.
//   bar4: low 94 ≤ 95 → "S" triggers while LONG. created(SHORT) != LONG →
//         reduce-only flip: close the long @95, stay FLAT, open nothing.
//
// EXPECTED (fixed): flat at end; two closed trades (SH round-trip @100/100,
// L round-trip @100/95). Pre-fix: "S" opens a phantom short @95 → position
// ends SHORT (pos_size = -1) with an extra open leg.
// ─────────────────────────────────────────────────────────────────────
static void test_carry_stop_flips_opposite_close_only() {
    std::printf("test_carry_stop_flips_opposite_close_only\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 1;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("SH", false, kNaN, kNaN, kNaN, "short setup");
            if (bar_index_ == 1 && position_side_ == PositionSide::SHORT)
                strategy_entry("S", false, kNaN, /*stop=*/95.0, kNaN, "carry short stop");
            if (bar_index_ == 2)
                strategy_entry("L", true, kNaN, kNaN, kNaN, "flip to long");
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100, 600'000),      // bar0: place SH
        mk(100, 100, 100, 100, 1'200'000),    // bar1: SH fills @100; arm S@95
        mk(100, 100, 100, 100, 1'800'000),    // bar2: place L
        mk(100, 100, 100, 100, 2'400'000),    // bar3: L fills @100 → LONG 1
        mk(100, 100,  94,  96, 3'000'000),    // bar4: S@95 triggers vs LONG
        mk( 96,  97,  95,  96, 3'600'000),    // bar5: settle
    };
    p.run(bars, 6);

    // Load-bearing: the carry stop closed the long WITHOUT opening a short.
    CHECK(near(p.pos_size(), 0.0));           // pre-fix: -1 (phantom short open)

    // Exactly two round trips: SH @100→@100, L @100→@95.
    CHECK(p.trade_count() == 2);
    if (p.trade_count() != 2) return;
    const Trade& sh = p.get_trade(0);
    const Trade& lt = p.get_trade(1);
    CHECK(near(sh.entry_price, 100.0));
    CHECK(near(sh.exit_price, 100.0));
    CHECK(near(lt.entry_price, 100.0));
    CHECK(near(lt.exit_price, 95.0));         // long closed at the stop level
    CHECK(lt.exit_bar_index == 4);
}

// ─────────────────────────────────────────────────────────────────────
// Guard: a SAME-cycle reverse must STILL open the new leg. Here the short
// stop "S" is armed while already LONG (created_position_side == LONG), so it
// is a normal in-position flip: closing the long AND opening a short is
// correct (created == reversed side → close_only gate does NOT fire).
//
//   bar0: place market long "L"
//   bar1: L fills @100 → LONG 1; arm "S" short stop @95 (created LONG)
//   bar4: low 94 ≤ 95 → "S" triggers: close long @95, open short 1 @95.
// EXPECTED: position ends SHORT 1 (a real flip, not close-only).
// ─────────────────────────────────────────────────────────────────────
static void test_same_cycle_reverse_still_opens() {
    std::printf("test_same_cycle_reverse_still_opens\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 1;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L", true, kNaN, kNaN, kNaN, "long setup");
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG)
                strategy_entry("S", false, kNaN, /*stop=*/95.0, kNaN, "same-cycle short stop");
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100, 600'000),      // bar0: place L
        mk(100, 100, 100, 100, 1'200'000),    // bar1: L fills @100; arm S@95 (created LONG)
        mk(100, 100, 100, 100, 1'800'000),
        mk(100, 100, 100, 100, 2'400'000),
        mk(100, 100,  94,  96, 3'000'000),    // bar4: S@95 triggers vs LONG
        mk( 96,  97,  95,  96, 3'600'000),
    };
    p.run(bars, 6);

    // A real flip: long closed, short opened. Position ends SHORT 1.
    CHECK(near(p.pos_size(), -1.0));
}

int main() {
    test_carry_stop_flips_opposite_close_only();
    test_same_cycle_reverse_still_opens();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
