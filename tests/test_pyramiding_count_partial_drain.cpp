/*
 * test_pyramiding_count_partial_drain.cpp — `pyramiding` bounds the number of
 * OCCUPIED ENTRY SLOTS in the current directional position, tested at
 * admission time. A slot is returned when the entry is retired by a CLOSE-PATH
 * order (strategy.close / close_all / reversal / broker close) and is NOT
 * returned when the entry is drained by a strategy.exit BRACKET leg fill.
 * Reaching flat releases every slot.
 *
 * Bug (pre-fix): settle_position_after_partial_exit() unconditionally
 * re-derived position_entry_count_ from pyramid_entries_.size(). A
 * strategy.exit bracket fill that DRAINED an entry leg while the position
 * stayed open therefore handed the pyramid slot back, and the next
 * same-direction market entry filled an add TradingView rejects.
 *
 * Ground truth (clause 4 — bracket drain PINS the slot) —
 * thulashimohanr-prev-day-week-levels-or-vwap-strategy, pyramiding=2,
 * ETH-USDT-USDT 15m (all times UTC):
 *
 *   2026-03-26 09:45  SHORT 2u @2082.49                     entry #1
 *   2026-03-26 17:45  ShortT1 limit 2042.49 fills 1u        (never flat)
 *   2026-03-27 09:45  SHORT 2u @2043.29                     entry #2  (TV admits)
 *   2026-03-27 10:30  limit 2003.30 fills 1u = the 03-26 remnant
 *                     -> the 03-26 leg drains; pyramid_entries_.size() 2 -> 1
 *   2026-03-29 09:45  SHORT would be entry #3 > pyramiding 2  TV REJECTS
 *
 * Both retirements of the 03-26 entry were `strategy.exit` T1 bracket fills,
 * i.e. the entry was FULLY closed and TV still refused the third entry.
 *
 * The price gate on 2026-03-29 is unambiguously true (09:30 close 1997.67 <
 * vwap 2003.236 and < orMid 2001.60) and the strategy.exit calls in the SAME
 * if-block did execute (the carried stops re-armed from orHigh(03-27)=2051.11
 * to orHigh(03-29)=2003.61 and fired at 11:00). Pine ran the block; TV's
 * broker emulator refused only the entry. Tape-wide rescan of
 * 2025-03-31..2026-04-30 confirms 2026-03-29 is the ONLY day TV skipped a
 * gate-satisfied OR entry, 15 entries occurred at streak=2 (all admitted) and
 * ZERO at streak=3.
 *
 * Ground truth (clause 3 — close-path retirement RELEASES the slot) —
 * 3commas-ena-grid-bot-long-strategy, pyramiding=200: 1021 entry fills over
 * only 64 REUSED entry ids, 776 entries accumulated between two flats, and
 * never more than 50 CONCURRENT open entries. Every exit is
 * strategy.close("L"+i) — zero strategy.exit calls in the script. TV admits
 * all 1021. A counter that never released would refuse 576 of them. The same
 * shape holds for the xau grid (pyramiding=50, 48 levels, 839 units traded).
 *
 * Fix: settle_position_after_partial_exit() takes the reduction cause. Only
 * PositionReductionCause::BRACKET_EXIT keeps the counter monotone
 * (std::max against pyramid_entries_.size()); every other cause re-derives it
 * from pyramid_entries_.size(). The cause is derived at the EXIT-order fill
 * site from the kClosePrefix ("__close__") id stamp that strategy.close /
 * close_all put on their materialised EXIT orders.
 *
 *   A. locus-2 shape: entry A 2u, BRACKET partial exit 1u, entry B 2u
 *      (count=2), BRACKET partial exit drains A's remnant, third
 *      same-direction MARKET entry is REJECTED — no fill event, no trade row,
 *      position untouched.
 *      (RED pre-fix: the drain reset count to 1 and the third entry filled.)
 *   B. counterfactual: after a full close the counter resets and the very
 *      same entry call admits again.
 *   C. a partial exit that does NOT drain a leg is inert either way (the
 *      second entry still admits) — pins that the fix only bites on drain.
 *   D. the grid-bot clause: A's exact shape but the draining exit is
 *      strategy.close("A", qty=1) instead of a bracket leg — the slot IS
 *      returned and the third same-direction entry is ADMITTED.
 *      (RED under the unconditional-monotone rule: count stays 2 and the
 *      third entry is refused, which is the 3commas-ena regression.)
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

namespace {

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

class PyramidProbe : public BacktestEngine {
public:
    PyramidProbe() {
        initial_capital_ = 1000000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        pyramiding_ = 2;
        process_orders_on_close_ = false;
    }

    std::string entry_id(int i) const { return closed_trade_entry_id(i); }
    std::string exit_id(int i) const { return closed_trade_exit_id(i); }
    double size(int i) const { return closed_trade_size(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double position_size() const { return signed_position_size(); }

    // Count closed rows whose entry lot came from a given entry id.
    int rows_for_entry(const std::string& id) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (closed_trade_entry_id(i) == id) ++n;
        }
        return n;
    }
};

// ── A/B: the locus-2 trace (every reduction is a strategy.exit BRACKET) ───
//
// X1/X2 are strategy.exit legs: their pending EXIT orders carry the plain ids
// "X1"/"X2", NOT the "__close__" stamp, so the fill site classifies both as
// PositionReductionCause::BRACKET_EXIT. The exit_id assertions below pin that
// — a rewrite that routed the drain through strategy.close would change them.
//
//  bar 0  signal entry A (2u)
//  bar 1  A fills @100                      pos 2u   [A:2]        count 1
//         signal exit X1 from A, limit 110, qty 1
//  bar 2  X1 fills 1u @110                  pos 1u   [A:1]        count 1
//         signal entry B (2u)
//  bar 3  B fills @100                      pos 3u   [A:1, B:2]   count 2
//         signal exit X2 from B, limit 120, qty 1
//  bar 4  X2 fills 1u @120 (FIFO -> drains A)  BRACKET_EXIT
//                                           pos 2u   [B:2]        count 2 (fixed)
//                                                                       1 (pre-fix)
//         signal entry C (2u)                       <- must be REJECTED
//  bar 5  C's fill attempt lands here
//  bar 6  (flat_reset only) close_all signalled
//  bar 7  close_all fills; signal entry D (2u)
//  bar 8  D fills                                   <- must be ADMITTED
//  bar 9  close_all signalled
//  bar 10 close_all fills
class DrainProbe : public PyramidProbe {
public:
    explicit DrainProbe(bool flat_reset_tail) : flat_reset_tail_(flat_reset_tail) {}

    void on_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0: strategy_entry("A", true, kNaN, kNaN, 2.0); break;
            case 1: strategy_exit("X1", "A", 110.0, kNaN, kNaN, kNaN, kNaN,
                                  100.0, "", 1.0); break;
            case 2: strategy_entry("B", true, kNaN, kNaN, 2.0); break;
            case 3: strategy_exit("X2", "B", 120.0, kNaN, kNaN, kNaN, kNaN,
                                  100.0, "", 1.0); break;
            case 4: strategy_entry("C", true, kNaN, kNaN, 2.0); break;
            case 6: if (flat_reset_tail_) strategy_close_all(); break;
            case 7: if (flat_reset_tail_) strategy_entry("D", true, kNaN, kNaN, 2.0);
                    break;
            case 9: if (flat_reset_tail_) strategy_close_all(); break;
            default: break;
        }
    }

private:
    bool flat_reset_tail_;
};

static std::vector<Bar> drain_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0
        mk(100, 101,  99, 100, 2000),   // 1  A fills @100
        mk(100, 111,  99, 100, 3000),   // 2  X1 limit 110
        mk(100, 101,  99, 100, 4000),   // 3  B fills @100
        mk(100, 121,  99, 100, 5000),   // 4  X2 limit 120 (drains A)
        mk(100, 101,  99, 100, 6000),   // 5  C's fill attempt
        mk(100, 101,  99, 100, 7000),   // 6
        mk(100, 101,  99, 100, 8000),   // 7
        mk(100, 101,  99, 100, 9000),   // 8
        mk(100, 101,  99, 100, 10000),  // 9
        mk(100, 101,  99, 100, 11000),  // 10
    };
}

}  // namespace

// ---- A: the drained pyramid slot is NOT handed back ------------------------

static void test_drained_leg_does_not_free_a_pyramid_slot() {
    std::printf("test_drained_leg_does_not_free_a_pyramid_slot\n");
    DrainProbe eng(/*flat_reset_tail=*/false);
    auto bars = drain_bars();
    eng.run(bars.data(), (int)bars.size());

    // Only the two partial-exit rows exist. Entry C never filled: no fill
    // event, no trade row, and the live position is still exactly B's 2 units.
    CHECK(eng.trade_count() == 2);
    CHECK(eng.entry_id(0) == std::string("A"));
    CHECK(eng.exit_id(0) == std::string("X1"));
    CHECK(near(eng.size(0), 1.0));
    CHECK(near(eng.exit_price(0), 110.0));
    CHECK(eng.entry_id(1) == std::string("A"));      // FIFO drains A's remnant
    // The draining exit is a strategy.exit BRACKET leg (no "__close__" stamp)
    // -> BRACKET_EXIT -> the slot stays occupied. Scenario D is the same shape
    // with a close-path drain and the opposite verdict.
    CHECK(eng.exit_id(1) == std::string("X2"));
    CHECK(near(eng.size(1), 1.0));
    CHECK(near(eng.exit_price(1), 120.0));
    CHECK(eng.rows_for_entry("C") == 0);
    CHECK(near(eng.position_size(), 2.0));           // pre-fix: 4.0
}

// ---- B: a full close resets the counter; the same call then admits ---------

static void test_flat_reset_readmits_the_entry() {
    std::printf("test_flat_reset_readmits_the_entry\n");
    DrainProbe eng(/*flat_reset_tail=*/true);
    auto bars = drain_bars();
    eng.run(bars.data(), (int)bars.size());

    // X1(1u from A) + X2(1u from A) + close_all(2u from B) + close_all(2u from D)
    CHECK(eng.trade_count() == 4);
    CHECK(eng.rows_for_entry("C") == 0);             // still rejected
    CHECK(eng.entry_id(2) == std::string("B"));
    CHECK(near(eng.size(2), 2.0));
    // D opened a fresh position after FLAT -> count reset to 1 -> admitted.
    CHECK(eng.entry_id(3) == std::string("D"));
    CHECK(near(eng.size(3), 2.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- C: a non-draining partial exit stays inert ---------------------------

namespace {

// Same shape, but the first partial exit takes only half of A's 2 units and
// the second entry follows immediately: no leg is ever drained, so the
// monotone counter and the size-derived counter agree throughout.
class NoDrainProbe : public PyramidProbe {
public:
    void on_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0: strategy_entry("A", true, kNaN, kNaN, 2.0); break;
            case 1: strategy_exit("X1", "A", 110.0, kNaN, kNaN, kNaN, kNaN,
                                  100.0, "", 1.0); break;
            case 2: strategy_entry("B", true, kNaN, kNaN, 2.0); break;
            case 4: strategy_close_all(); break;
            default: break;
        }
    }
};

}  // namespace

static void test_partial_exit_without_drain_is_inert() {
    std::printf("test_partial_exit_without_drain_is_inert\n");
    NoDrainProbe eng;
    auto bars = drain_bars();
    eng.run(bars.data(), (int)bars.size());

    // A(2u) - 1u exit = 1u remnant, then B(2u) admits (entry #2 <= 2).
    // close_all on bar 4 flushes both surviving lots at bar 5's open.
    CHECK(eng.trade_count() == 3);
    CHECK(eng.exit_id(0) == std::string("X1"));
    CHECK(near(eng.size(0), 1.0));
    CHECK(eng.entry_id(1) == std::string("A"));
    CHECK(near(eng.size(1), 1.0));
    CHECK(eng.entry_id(2) == std::string("B"));
    CHECK(near(eng.size(2), 2.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- D: a CLOSE-PATH drain DOES free the pyramid slot ---------------------

namespace {

// ── D: scenario A's shape, with strategy.close doing the draining ─────────
//
// The grid-bot clause. 3commas-ena reuses 64 entry ids for 1021 fills and
// retires every one with strategy.close(); TV never refuses an entry even
// though 776 accumulate between flats under a cap of 200, because occupancy
// peaks at 50. A rule that pinned the slot on ANY reduction would refuse 576
// TV-admitted entries.
//
//  bar 0  signal entry A (2u)
//  bar 1  A fills @100                      pos 2u   [A:2]        count 1
//         signal exit X1 from A, limit 110, qty 1   (BRACKET, non-draining)
//  bar 2  X1 fills 1u @110                  pos 1u   [A:1]        count 1
//         signal entry B (2u)
//  bar 3  B fills @100                      pos 3u   [A:1, B:2]   count 2
//         signal strategy.close("A", qty=1) -> deferred EXIT "__close__A"
//  bar 4  "__close__A" fills 1u @open 100 (FIFO -> drains A's remnant)
//                                           pos 2u   [B:2]        count 1
//         signal entry C (2u)                       <- must be ADMITTED
//  bar 5  C fills @100                      pos 4u   [B:2, C:2]   count 2
//  bar 6  close_all signalled
//  bar 7  close_all fills -> flat
class CloseDrainProbe : public PyramidProbe {
public:
    void on_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0: strategy_entry("A", true, kNaN, kNaN, 2.0); break;
            case 1: strategy_exit("X1", "A", 110.0, kNaN, kNaN, kNaN, kNaN,
                                  100.0, "", 1.0); break;
            case 2: strategy_entry("B", true, kNaN, kNaN, 2.0); break;
            case 3: strategy_close("A", "drain", /*qty=*/1.0); break;
            case 4: strategy_entry("C", true, kNaN, kNaN, 2.0); break;
            case 6: strategy_close_all(); break;
            default: break;
        }
    }
};

}  // namespace

static void test_close_path_drain_frees_a_pyramid_slot() {
    std::printf("test_close_path_drain_frees_a_pyramid_slot\n");
    CloseDrainProbe eng;
    auto bars = drain_bars();
    eng.run(bars.data(), (int)bars.size());

    // X1(A 1u @110) + __close__A(A 1u @100) + close_all(B 2u) + close_all(C 2u)
    CHECK(eng.trade_count() == 4);

    CHECK(eng.entry_id(0) == std::string("A"));
    CHECK(eng.exit_id(0) == std::string("X1"));       // bracket leg, no drain
    CHECK(near(eng.size(0), 1.0));
    CHECK(near(eng.exit_price(0), 110.0));

    // The DRAIN of A's remnant travels the close path: the materialised EXIT
    // order carries the kClosePrefix stamp, so the reduction cause is
    // SCRIPT_ORDER and the pyramid slot is returned.
    CHECK(eng.entry_id(1) == std::string("A"));
    CHECK(eng.exit_id(1) == std::string("__close__A"));
    CHECK(near(eng.size(1), 1.0));
    CHECK(near(eng.exit_price(1), 100.0));

    // The third same-direction entry is ADMITTED — this is exactly the cell
    // the unconditional-monotone rule got wrong (RED there: 0 rows for C,
    // trade_count 3, final position 2u).
    CHECK(eng.rows_for_entry("C") == 1);
    CHECK(eng.entry_id(2) == std::string("B"));
    CHECK(near(eng.size(2), 2.0));
    CHECK(eng.entry_id(3) == std::string("C"));
    CHECK(near(eng.size(3), 2.0));
    CHECK(near(eng.position_size(), 0.0));
}

int main() {
    std::printf("=== test_pyramiding_count_partial_drain ===\n");

    test_drained_leg_does_not_free_a_pyramid_slot();
    test_flat_reset_readmits_the_entry();
    test_partial_exit_without_drain_is_inert();
    test_close_path_drain_frees_a_pyramid_slot();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
