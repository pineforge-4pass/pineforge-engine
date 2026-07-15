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

// ──────────────────────────────────────────────────────────────────
// Same-cycle frozen reversal transaction, exact-equality cell.
//
// A priced explicit-FIXED opposite entry placed while holding H contracts
// freezes a broker transaction of H + Q.  If same-direction adds grow the
// live position to exactly that frozen transaction before the priced order
// fills, TradingView consumes the whole transaction closing the live position
// and has no remainder with which to open the requested side.
//
//   bar0: place market L1 qty 1
//   bar1: L1 fills -> LONG 1; arm S stop qty 1 (H=1, frozen tx=2), then
//         place same-direction market L2 qty 1
//   bar2: L2 fills -> live LONG 2
//   bar3: S triggers; live 2 == frozen tx 2 -> close both longs, stay FLAT
//
// Pre-fix the ordinary same-cycle reversal path closes both longs and opens a
// fresh SHORT 1.  This is the campaign's seven-row M2 residual in minimal form.
// ──────────────────────────────────────────────────────────────────
static void test_same_cycle_frozen_transaction_exactly_flattens_long() {
    std::printf("test_same_cycle_frozen_transaction_exactly_flattens_long\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 2;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("L1", true, kNaN, kNaN, 1.0, "base long");
            }
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("S", false, kNaN, /*stop=*/95.0, 1.0,
                               "frozen short reversal");
                strategy_entry("L2", true, kNaN, kNaN, 1.0,
                               "intervening long add");
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),  // L1 fills; place S then L2
        mk(100, 100, 100, 100, 1'800'000),  // L2 fills; S remains untouched
        mk(100, 100,  94,  96, 2'400'000),  // S triggers against live LONG 2
        mk( 96,  97,  95,  96, 3'000'000),
    };
    p.run(bars, 5);

    CHECK(near(p.pos_size(), 0.0));            // RED: pre-fix ends SHORT 1
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(p.get_trade(0).is_long);
        CHECK(p.get_trade(1).is_long);
        CHECK(near(p.get_trade(0).qty, 1.0));
        CHECK(near(p.get_trade(1).qty, 1.0));
        CHECK(near(p.get_trade(0).exit_price, 95.0));
        CHECK(near(p.get_trade(1).exit_price, 95.0));
    }
}

// Mirrored exact-equality cell: SHORT 1, arm long stop Q=1, add SHORT 1,
// then fill against live SHORT 2.  The frozen transaction is also 2, so the
// fill closes both shorts and opens no long remainder.
static void test_same_cycle_frozen_transaction_exactly_flattens_short() {
    std::printf("test_same_cycle_frozen_transaction_exactly_flattens_short\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 2;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("S1", false, kNaN, kNaN, 1.0, "base short");
            }
            if (bar_index_ == 1 && position_side_ == PositionSide::SHORT) {
                strategy_entry("L", true, kNaN, /*stop=*/105.0, 1.0,
                               "frozen long reversal");
                strategy_entry("S2", false, kNaN, kNaN, 1.0,
                               "intervening short add");
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 100, 100, 100, 1'800'000),
        mk(100, 106, 100, 104, 2'400'000),
        mk(104, 105, 103, 104, 3'000'000),
    };
    p.run(bars, 5);

    CHECK(near(p.pos_size(), 0.0));            // RED: pre-fix ends LONG 1
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(!p.get_trade(0).is_long);
        CHECK(!p.get_trade(1).is_long);
        CHECK(near(p.get_trade(0).exit_price, 105.0));
        CHECK(near(p.get_trade(1).exit_price, 105.0));
    }
}

// Mutation control: an explicit-FIXED priced reversal with no intervening add
// has live=1 while its placement-frozen transaction is H+Q=2. The new rule is
// equality-only, so live<frozen retains the legacy close-and-open reversal.
static void test_same_cycle_explicit_fixed_live_less_than_frozen_unchanged() {
    std::printf("test_same_cycle_explicit_fixed_live_less_than_frozen_unchanged\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 2;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L", true, kNaN, kNaN, 1.0);
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG)
                strategy_entry("S", false, kNaN, /*stop=*/95.0, 1.0);
        }
    };
    Probe p;
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 100,  94,  96, 1'800'000),
        mk( 96,  97,  95,  96, 2'400'000),
    };
    p.run(bars, 4);

    CHECK(near(p.pos_size(), -1.0));
    CHECK(p.trade_count() == 1);
}

// Mutation control: a finite quantity is not necessarily FIXED. With CASH
// sizing the raw value 1 would make H+raw=2 look equal to live=2, but the
// actual own leg is cash/fill_price. The equality exception must stay scoped
// to explicit FIXED sizing; this fill closes both longs and opens a small short.
static void test_same_cycle_finite_cash_qty_unchanged() {
    std::printf("test_same_cycle_finite_cash_qty_unchanged\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 2;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L1", true, kNaN, kNaN, 1.0);
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("S", false, kNaN, /*stop=*/95.0, 1.0,
                               "cash-sized reversal", "", 0,
                               static_cast<int>(QtyType::CASH));
                strategy_entry("L2", true, kNaN, kNaN, 1.0);
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 100, 100, 100, 1'800'000),
        mk(100, 100,  94,  96, 2'400'000),
        mk( 96,  97,  95,  96, 3'000'000),
    };
    p.run(bars, 5);

    CHECK(p.pos_size() < 0.0);
    CHECK(std::abs(p.pos_size()) < 0.1);
    CHECK(p.trade_count() == 2);
}

// Mutation control: side equality is not cycle identity. S is armed in the
// first LONG cycle (H=1/Q=1), survives a LONG -> SHORT -> fresh LONG2 sequence,
// then triggers with live=2. Although side and quantity equal the positive
// cell, the order predates this position instance and keeps legacy reversal
// behavior, ending SHORT1 rather than close-only FLAT.
static void test_double_flip_same_side_is_not_same_position_cycle() {
    std::printf("test_double_flip_same_side_is_not_same_position_cycle\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 2;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L", true, kNaN, kNaN, 1.0);
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("S", false, kNaN, /*stop=*/90.0, 1.0);
                strategy_entry("F", false, kNaN, kNaN, 1.0);
            }
            if (bar_index_ == 2 && position_side_ == PositionSide::SHORT)
                strategy_entry("G", true, kNaN, kNaN, 2.0);
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),  // L fills; arm S and queue F
        mk(100, 100, 100, 100, 1'800'000),  // F fills -> SHORT1; queue G
        mk(100, 100, 100, 100, 2'400'000),  // G fills -> fresh LONG2
        mk(100, 100,  89,  91, 3'000'000),  // old S triggers against LONG2
        mk( 91,  92,  90,  91, 3'600'000),
    };
    p.run(bars, 6);

    CHECK(near(p.pos_size(), -1.0));          // side-only patch: 0.0
    CHECK(p.trade_count() == 3);
}

// Mutation control: MARKET reversals do not carry a priced-order frozen
// transaction and must retain ordinary close-and-open behavior.
static void test_market_same_cycle_reversal_unchanged() {
    std::printf("test_market_same_cycle_reversal_unchanged\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 1;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L", true, kNaN, kNaN, 1.0, "long");
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG)
                strategy_entry("S", false, kNaN, kNaN, 1.0,
                               "ordinary market reversal");
        }
    };
    Probe p;
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 100, 100, 100, 1'800'000),
        mk(100, 100, 100, 100, 2'400'000),
    };
    p.run(bars, 4);

    CHECK(near(p.pos_size(), -1.0));
    CHECK(p.trade_count() == 1);
}

// Mutation control: equality is the only newly pinned size relation.  With
// H=1/Q=1 but two intervening adds, live=3 > frozen transaction 2; retain the
// legacy full reversal to SHORT 1 rather than inferring a partial reduction.
static void test_same_cycle_live_greater_than_frozen_unchanged() {
    std::printf("test_same_cycle_live_greater_than_frozen_unchanged\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 3;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L1", true, kNaN, kNaN, 1.0);
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("S", false, kNaN, /*stop=*/95.0, 1.0);
                strategy_entry("L2", true, kNaN, kNaN, 1.0);
                strategy_entry("L3", true, kNaN, kNaN, 1.0);
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 100, 100, 100, 1'800'000),
        mk(100, 100,  94,  96, 2'400'000),
        mk( 96,  97,  95,  96, 3'000'000),
    };
    p.run(bars, 5);

    CHECK(near(p.pos_size(), -1.0));
    CHECK(p.trade_count() == 3);
}

// Mutation control: even when H + the FIXED default happens to equal the live
// position, qty=na is not an explicit-FIXED oracle cell and stays on the
// legacy full-reversal path.
static void test_default_fixed_exact_size_unchanged() {
    std::printf("test_default_fixed_exact_size_unchanged\n");
    class Probe : public BacktestEngine {
    public:
        Probe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 2;
            syminfo_mintick_ = 0.01;
        }
        double pos_size() const { return signed_position_size(); }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0)
                strategy_entry("L1", true, kNaN, kNaN, 1.0);
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("S", false, kNaN, /*stop=*/95.0, kNaN);
                strategy_entry("L2", true, kNaN, kNaN, 1.0);
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 100, 100, 100, 1'200'000),
        mk(100, 100, 100, 100, 1'800'000),
        mk(100, 100,  94,  96, 2'400'000),
        mk( 96,  97,  95,  96, 3'000'000),
    };
    p.run(bars, 5);

    CHECK(near(p.pos_size(), -1.0));
    CHECK(p.trade_count() == 2);
}

int main() {
    test_carry_stop_flips_opposite_close_only();
    test_same_cycle_reverse_still_opens();
    test_same_cycle_frozen_transaction_exactly_flattens_long();
    test_same_cycle_frozen_transaction_exactly_flattens_short();
    test_same_cycle_explicit_fixed_live_less_than_frozen_unchanged();
    test_same_cycle_finite_cash_qty_unchanged();
    test_double_flip_same_side_is_not_same_position_cycle();
    test_market_same_cycle_reversal_unchanged();
    test_same_cycle_live_greater_than_frozen_unchanged();
    test_default_fixed_exact_size_unchanged();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
