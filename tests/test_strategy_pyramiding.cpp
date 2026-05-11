/*
 * test_strategy_pyramiding.cpp — verify TradingView's deferred-flip
 * carry consumption rule on BacktestEngine.
 *
 * Background: a priced (stop/limit) entry placed while a position was
 * open captures that position's qty into ``PendingOrder::tv_carry_qty``
 * at placement time. If the source position is later closed and the
 * priced entry now fires from FLAT in the OPPOSITE direction, TV opens
 * the new position at ``base_qty + tv_carry_qty`` (validation/52, 63,
 * 72, 92, 95, 96 chains). Sibling priced entries (same created_position
 * cycle, same direction) must lose their carry when one fires from flat
 * — otherwise probe 93 (pyramiding=2, two opposite-direction stops
 * armed during separate long cycles) double-grows.
 *
 * The cycle-scope predicate is the load-bearing piece: a sibling armed
 * in a LATER cycle (created_bar > firing order's created_bar) captures
 * carry from a DIFFERENT source position; it owns its carry and TV does
 * not pre-emptively wipe it. Without that scope, the next-cycle sibling
 * fires fresh and the qty schedule shifts by one full cycle, leaking
 * ~qty × mintick of per-leg PnL drift across the whole chain (probe 95
 * is the oracle: open-guaranteed stops eliminate sub-bar precision so
 * any mismatch must come from the carry rule itself).
 */

#include <cassert>
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

namespace {

// Open-guaranteed flip-stop probe — same shape as
// validation/95-multi-cycle-open-guaranteed-stops:
//   - bar 1 (down):  enter SE short stop @ high*10, cancel LE
//   - bar 2 (up):    enter LE long stop @ low*0.1, cancel SE,
//                    close prior short if any
// Stop levels are engineered so the fill price is always next bar's
// open (high*10 always >= low → SE fills at min(open, high*10)=open;
// low*0.1 always <= high → LE fills at max(open, low*0.1)=open). This
// makes the fill price independent of any sub-bar path so per-leg PnL
// drift can only come from the carry-qty schedule.
class DeferredFlipProbe : public BacktestEngine {
public:
    struct TradeRow { std::string entry_id; double qty; double pnl; };
    std::vector<TradeRow> closed_trades;

    DeferredFlipProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 1;
        // The deferred-flip rule is gated on the script having a
        // ``strategy.close`` call at compile time. The codegen would
        // set this flag from an AST scan; here we set it directly.
        script_has_strategy_close_ = true;
    }

    void on_bar(const Bar& bar) override {
        bool isDown = (bar_index_ % 2) == 0;  // bars 0,2,4,... are "down"
        bool isUp   = (bar_index_ % 2) == 1;  // bars 1,3,5,... are "up"

        if (isDown) {
            // Short stop priced at high*10 — guaranteed to fill at next
            // bar's open (low <= high*10 always true).
            strategy_entry("SE", /*is_long=*/false,
                           std::numeric_limits<double>::quiet_NaN(),
                           /*stop_price=*/bar.high * 10.0,
                           /*qty=*/1.0,
                           "open-guaranteed short");
            strategy_cancel("LE");
        }
        if (isUp) {
            strategy_entry("LE", /*is_long=*/true,
                           std::numeric_limits<double>::quiet_NaN(),
                           /*stop_price=*/bar.low * 0.1,
                           /*qty=*/1.0,
                           "open-guaranteed long");
            strategy_cancel("SE");
        }
        // Daily flip-flat closes happen AFTER the entries are placed,
        // mirroring probe 95 source order. Empty id closes everything.
        if (isDown && position_side_ == PositionSide::LONG) {
            strategy_close("LE", "flip flat long");
        }
        if (isUp && position_side_ == PositionSide::SHORT) {
            strategy_close("SE", "flip flat short");
        }

        // Snapshot at run end so the test can inspect the closed trades
        // after the bar loop returns. Subclass has access to protected
        // ``trades_``; the harness does not.
        last_bar_index_seen = bar_index_;
        closed_trades.clear();
        for (const auto& t : trades_) {
            closed_trades.push_back({t.entry_id, t.qty, t.pnl});
        }
    }

    int last_bar_index_seen = -1;
};

}  // namespace

// Scenario 1: deferred-flip oracle (probe 95-style). Cycle qty grows
// 1, 1, 2, 2, 3, 3, ... — each cycle pair is a flip from prior side
// followed by a fresh entry of the next-larger size. With the carry
// rule working correctly, the qty chain ascends; without it, qty stays
// stuck at 1.
static void test_deferred_flip_chain_grows() {
    std::printf("test_deferred_flip_chain_grows\n");
    DeferredFlipProbe p;

    // Build a simple OHLCV: each bar is a tight 2-tick range so high*10
    // and low*0.1 produce extreme stops. Open price drifts so each
    // cycle's fill price differs.
    constexpr int N = 12;
    Bar bars[N];
    double open_price = 100.0;
    for (int i = 0; i < N; ++i) {
        bars[i].open  = open_price;
        bars[i].high  = open_price + 1.0;
        bars[i].low   = open_price - 1.0;
        bars[i].close = open_price;
        bars[i].volume = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 60'000;
        open_price += 5.0;
    }

    p.run(bars, N);

    // First entry should be qty=1 (no prior carry). Each subsequent
    // entry from FLAT after a strategy.close should grow by previous
    // qty (carry).
    CHECK(p.closed_trades.size() >= 4);

    // Check that qty chain grows (the carry rule must apply at least
    // once). This catches the bug where mis-cycled carry would leave
    // qty stuck at 1.
    int max_qty = 0;
    for (const auto& tr : p.closed_trades) {
        if ((int)tr.qty > max_qty) max_qty = (int)tr.qty;
    }
    CHECK(max_qty >= 2);
}

// Scenario 2: deferred-flip is gated on opposite direction. A
// pre-armed SAME-direction priced entry (long stop placed during a
// long, position closes, long stop later fires from flat) should NOT
// apply carry — TV only flips qty when the new direction is OPPOSITE
// to the carry source.
static void test_same_direction_no_carry() {
    std::printf("test_same_direction_no_carry\n");
    class SameDirProbe : public BacktestEngine {
    public:
        struct TradeRow { std::string entry_id; double qty; };
        std::vector<TradeRow> closed_trades;
        double final_position_qty = 0.0;
        PositionSide final_position_side = PositionSide::FLAT;

        SameDirProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 1;
            script_has_strategy_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            if (bar_index_ == 0) {
                strategy_entry("L1", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               2.0, "first long qty 2");
            }
            // Bar 1: while long-2, place SAME-direction long stop +
            // close current long. Stop fires next bar from flat.
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("L2", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/bar.low * 0.1,
                               1.0, "L2 same dir stop");
                strategy_close("L1", "close first long");
            }
            // Bar 4: snapshot final state.
            if (bar_index_ == 4) {
                final_position_qty = position_qty_;
                final_position_side = position_side_;
                for (const auto& t : trades_) {
                    closed_trades.push_back({t.entry_id, t.qty});
                }
            }
        }
    };
    SameDirProbe p;
    Bar bars[5] = {
        {100, 101,  99, 100, 1000,  60'000},
        {100, 101,  99, 100, 1000, 120'000},  // place L2 + close L1
        {100, 101,  99, 100, 1000, 180'000},  // L1 close fires; L2 fires from flat — qty should be 1
        {100, 101,  99, 100, 1000, 240'000},
        {100, 101,  99, 100, 1000, 300'000},
    };
    p.run(bars, 5);

    // L2 should have qty 1 (no carry applied since direction == carry-source direction).
    // Either it closed with qty 1, or it's the open position with qty 1
    // (the test bars are too short to drive a separate exit).
    bool found_l2 = false;
    for (const auto& tr : p.closed_trades) {
        if (tr.entry_id == "L2") {
            CHECK(near(tr.qty, 1.0));
            found_l2 = true;
        }
    }
    if (!found_l2 && p.final_position_side == PositionSide::LONG) {
        // L2 fired and is open at end. Verify no carry leaked.
        CHECK(near(p.final_position_qty, 1.0));
    }
}

// Scenario 3: cycle-scope sibling carry independence. Two short stops
// armed in DIFFERENT position cycles. The earlier sibling fires while
// the later sibling is still pending — without the cycle-scope guard,
// the firing sibling's consume call zeros the later sibling's carry,
// so when the later sibling eventually fires from flat it opens at
// just qty=base instead of qty=base+carry.
//
// Trade shape (two pending shorts coexist when the first fires):
//   bar 0: long LA opened (qty=1).
//   bar 1: place A_far short stop priced FAR BELOW the bar range
//          (stop=1.0) so it cannot fire on bar 2. close LA. A_far
//          captures carry=1 (cycle A), created_bar=1.
//   bar 2: LA closes. A_far still pending (low=99 > stop=1).
//   bar 3: long LB opened (qty=1).
//   bar 4: place B_close short stop priced JUST BELOW low (stop=low-0.5).
//          close LB. B_close captures carry=1 (cycle B), created_bar=4.
//          Position is still LONG (close is queued for next bar).
//   bar 5: LB close fires at open. B_close stop is below this bar's
//          low, so it fires THIS bar from flat. consume(B_close) walks
//          pending_orders; A_far is a same-direction sibling. With the
//          scope guard, A_far's created_bar (1) is < B_close's
//          created_bar (4), so the guard does NOT skip — A_far IS
//          consumed.
//
// To test the OPPOSITE direction (sibling placed LATER preserved when
// EARLIER one fires), we need:
//   bar 1: place EARLY short S_early with stop in range. close LA.
//   bar 2: S_early fires from flat. consume(S_early) walks pending; we
//          want a LATER-placed sibling preserved.
// But on bar 2 the later sibling doesn't exist yet (cycle B hasn't
// started). So this only matters when:
//   - cycle A places S with FAR stop (won't fire bar 2)
//   - cycle B starts, places S2 with NEAR stop
//   - then S's stop is touched LATER
// In that case, S firing must NOT consume S2's carry.
//
// The test below uses this exact pattern. We make A_in_range start as
// the EARLIER sibling and engineer prices so it fires LATER (after B's
// placement). After A fires, B should still have its carry.
static void test_two_cycle_siblings_independent_carry() {
    std::printf("test_two_cycle_siblings_independent_carry\n");
    // Engineer two short-stop siblings with DIFFERENT created_bars but
    // both still pending when one of them fires from flat. The earlier
    // sibling is given a stop in range so it fires AFTER the later
    // sibling has been placed.
    //
    // Setup:
    //   bar 0: long LA opened (qty=1).
    //   bar 1: arm A short stop at price 90 (close stop, but bar lows
    //          stay above 90 until bar 6) + close LA. A captures
    //          carry=1, created_bar=1.
    //   bar 2: LA closes.
    //   bar 3: long LB opened (qty=1).
    //   bar 4: arm B short stop at price 1 (won't fire) + close LB. B
    //          captures carry=1, created_bar=4. Both A and B now
    //          pending; A.created_bar (1) < B.created_bar (4).
    //   bar 5: LB closes.
    //   bar 6: price drops; A's stop=90 is touched → A fires from FLAT
    //          with carry=1+1=2. consume_tv_carry_from_siblings(A)
    //          walks pending_orders_, finds B with same direction,
    //          same created_position_side=LONG. Without the scope
    //          guard: B.tv_carry_qty zeroed. With the scope guard:
    //          B.created_bar (4) > A.created_bar (1) → guard skips B,
    //          B's carry is preserved.
    //   bar 7+: snapshot B's tv_carry_qty in pending_orders_. With the
    //          fix, B still has carry=1.
    class TwoCycleSiblingProbe : public BacktestEngine {
    public:
        struct PendingSnap { std::string id; double carry; int created_bar; };
        std::vector<PendingSnap> pending_at_end;

        TwoCycleSiblingProbe() {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            slippage_ = 0;
            commission_value_ = 0;
            pyramiding_ = 5;
            script_has_strategy_close_ = true;
        }
        void on_bar(const Bar& bar) override {
            (void)bar;
            if (bar_index_ == 0) {
                strategy_entry("LA", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               1.0, "long A");
            }
            if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
                strategy_entry("A", false,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/90.0,
                               1.0, "A short — stops at 90");
                strategy_close("LA", "close long A");
            }
            if (bar_index_ == 3 && position_side_ == PositionSide::FLAT) {
                strategy_entry("LB", true,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::quiet_NaN(),
                               1.0, "long B");
            }
            if (bar_index_ == 4 && position_side_ == PositionSide::LONG) {
                strategy_entry("B", false,
                               std::numeric_limits<double>::quiet_NaN(),
                               /*stop=*/1.0,
                               1.0, "B short — far stop");
                strategy_close("LB", "close long B");
            }
            // Bar 7 snapshot — A should have fired by now (bar 6 had a
            // price drop touching A's stop 90); B should still be
            // pending with its carry intact thanks to the scope guard.
            if (bar_index_ == 7) {
                for (const auto& po : pending_orders_) {
                    pending_at_end.push_back({po.id, po.tv_carry_qty,
                                               po.created_bar});
                }
            }
        }
    };
    TwoCycleSiblingProbe p;
    Bar bars[8];
    // Bars 0..5: prices ~100±1, lows above 90.
    // Bar 6: price drop, low 85 → A's stop=90 triggers.
    // Bar 7: prices recover to ~95.
    double opens[8]      = { 100, 100, 100, 100, 100, 100, 95, 95 };
    double highs[8]      = { 101, 101, 101, 101, 101, 101, 96, 96 };
    double lows[8]       = {  99,  99,  99,  99,  99,  99, 85, 94 };
    double closes[8]     = { 100, 100, 100, 100, 100, 100, 90, 95 };
    for (int i = 0; i < 8; ++i) {
        bars[i].open      = opens[i];
        bars[i].high      = highs[i];
        bars[i].low       = lows[i];
        bars[i].close     = closes[i];
        bars[i].volume    = 1000.0;
        bars[i].timestamp = (int64_t)(i + 1) * 60'000;
    }
    p.run(bars, 8);

    bool b_seen = false;
    for (const auto& ps : p.pending_at_end) {
        if (ps.id == "B") {
            b_seen = true;
            // With the cycle-scope guard: B's carry preserved (=1).
            // Without the guard: B's carry zeroed (=0).
            CHECK(near(ps.carry, 1.0));
        }
    }
    CHECK(b_seen);
}

int main() {
    test_deferred_flip_chain_grows();
    test_same_direction_no_carry();
    test_two_cycle_siblings_independent_carry();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
