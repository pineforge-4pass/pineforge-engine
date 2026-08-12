/*
 * test_exit_bracket_position_cycle_lifetime.cpp — finding-347. A from_entry
 * bracket leg lives for the POSITION cycle, not for its own entry bucket.
 *
 * Bug (pre-fix): classify_order_eligibility tested pyramid_entries_ RESIDENCY
 * to decide whether a from_entry-bound exit was still live. When a sibling
 * bracket FIFO-consumed all of a leg's own units, no pyramid entry carried that
 * entry_id any more and the next eligibility pass Removed the leg permanently.
 * The engine then fired 3 of 4 bracket legs, carried a phantom unit and never
 * reached flat.
 *
 * TV's rule: from_entry decides only whether a leg is ALLOWED TO EXIST (its
 * parent entry must have filled in this position cycle), never which units it
 * may take — those come from the position-level FIFO queue. The engine's FILL
 * path already drew FIFO across buckets; only the eligibility gate was
 * bucket-scoped, which is what makes the fix surgical.
 *
 * Ground truth — thulashimohanr-prev-day-week-levels-or-vwap-strategy,
 * ETH-USDT-USDT 15m (UTC). 2025-06-17 09:45 fills BOTH entry ids on one bar
 * (`Short` 2u + `ShortAdd` 2u = 4 units, 4 bracket legs priced off the same
 * 09:30 close). TV's CROSS-ASSIGNED exit labels are the direct proof:
 *
 *   #127 06-17 14:45  T1 Exit   2525.91 q1   entry ▼ SHORT     (Short's leg,    Short unit)
 *   #128 06-17 14:45  Add T1    2525.91 q1   entry ▼ SHORT     <- ShortAdd's leg took a Short unit
 *   #129 06-17 16:45  T2 Exit   2465.91 q1   entry ▼+ ADD      <- Short's leg took a ShortAdd unit
 *   #130 06-17 16:45  Add T2    2465.91 q1   entry ▼+ ADD
 *                                            ==> 4 in, 4 out: TV is FLAT 16:45
 *
 * The T1 pair drains BOTH `Short` units (engine agrees), orphaning `ShortT2`.
 * TV still fires it at 16:45; the engine Removed it and fired only `ShortAddT2`
 * — one unit instead of two. Identical at 2025-10-14 and 2026-01-14. Exactly 3
 * of the window's 12 two-id bars diverge: the 9 that exit all four legs on ONE
 * bar match, because no eligibility pass runs inside the orphaning window.
 *
 * Fix: replace bucket residency with position-cycle provenance
 * (cycle_filled_entry_ids_, cleared on flat / fresh open).
 *
 *   A. the 06-17 shape — all FOUR units exit, 2 at the T1 price and 2 at the
 *      T2 price, including the ORPHANED bucket's leg; engine flat afterwards.
 *      (RED pre-fix: 3 rows, 1 phantom unit left open.)
 *   B. the 06-18 shape — because A now reaches flat, the next day's same-id
 *      entry is ADMITTED. This is the p1 (monotone pyramiding counter)
 *      interaction pin: p1 asks the right question, and only D3 gives it the
 *      right position to ask it about.
 *   C. negative — a leg whose from_entry NEVER filled in this cycle is still
 *      Removed (the gate still has teeth).
 *   D. the Remove path's original purpose survives: after a full close, a
 *      stale leg from the prior cycle does not fire against the new position.
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

class CycleProbe : public BacktestEngine {
public:
    CycleProbe() {
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
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double size(int i) const { return closed_trade_size(i); }
    double position_size() const { return signed_position_size(); }

    int rows_with_exit_id(const std::string& xid) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (closed_trade_exit_id(i) == xid) ++n;
        }
        return n;
    }
    int rows_at_exit_price(double px) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (near(closed_trade_exit_price(i), px)) ++n;
        }
        return n;
    }
};

// ── A + B: the 2025-06-17 / 06-18 pair ────────────────────────────────────
//
//  bar 0  entry S(2u) + entry SA(2u), and all FOUR bracket legs:
//         T1(from S, lim 96)  T2(from S, lim 90)
//         AT1(from SA,lim 96) AT2(from SA,lim 90)
//  bar 1  BOTH entries fill @100          pos -4  [S:2, SA:2]  count 2
//  bar 2  low 95 -> T1 and AT1 both fill @96, FIFO drains BOTH S units
//                                         pos -2  [SA:2]
//         -> the S bucket is now empty: pre-fix, T2 is Removed here
//  bar 3  low 89 -> T2 AND AT2 fill @90   pos 0   FLAT
//         T2's from_entry is "S" but the unit it takes is an SA unit — TV's
//         cross-assigned "T2 Exit" on a ▼+ ADD entry.
//  bar 4  entry S(2u) again — admitted, because bar 3 reached flat
//  bar 5  it fills @90                    pos -2
//  bar 6  close_all
//  bar 7  it fills                        pos 0
class TwoBucketProbe : public CycleProbe {
public:
    void on_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                strategy_entry("S", false, kNaN, kNaN, 2.0);
                strategy_entry("SA", false, kNaN, kNaN, 2.0);
                strategy_exit("T1", "S", 96.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", 1.0);
                strategy_exit("T2", "S", 90.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", 1.0);
                strategy_exit("AT1", "SA", 96.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", 1.0);
                strategy_exit("AT2", "SA", 90.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", 1.0);
                break;
            case 4: strategy_entry("S", false, kNaN, kNaN, 2.0); break;
            case 6: strategy_close_all(); break;
            default: break;
        }
    }
};

static std::vector<Bar> two_bucket_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0
        mk(100, 101,  99, 100, 2000),   // 1  S + SA both fill @100
        mk(100, 101,  95,  96, 3000),   // 2  T1 + AT1 @96 (drain the S bucket)
        mk( 96,  97,  89,  90, 4000),   // 3  T2 + AT2 @90 -> FLAT
        mk( 90,  91,  89,  90, 5000),   // 4  next-day entry signal
        mk( 90,  91,  89,  90, 6000),   // 5  it fills @90
        // The cycle-2 close_all deliberately settles at a level distinct from
        // both bracket prices so the per-price row census stays unambiguous.
        mk( 85,  86,  84,  85, 7000),   // 6  close_all
        mk( 85,  86,  84,  85, 8000),   // 7  it fills @85
        mk( 85,  86,  84,  85, 9000),   // 8
    };
}

// ── C: a leg bound to an entry id that never filled is still Removed ───────
class GhostLegProbe : public CycleProbe {
public:
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("S", false, kNaN, kNaN, 2.0);
        // "NEVER" is never issued as an entry: this leg must never fire, even
        // though its limit is touched on bar 2.
        strategy_exit("GHOST", "NEVER", 96.0, kNaN, kNaN, kNaN, kNaN,
                      100.0, "", 1.0);
        strategy_exit("REAL", "S", 90.0, kNaN, kNaN, kNaN, kNaN,
                      100.0, "", 2.0);
    }
};

// ── D: stale legs from a prior cycle do not fire against the new position ──
//
// Two variants: the new cycle re-uses the entry id, and the new cycle uses a
// different one. The "X" leg's limit (80) is only reachable in cycle 2.
class StaleLegProbe : public CycleProbe {
public:
    explicit StaleLegProbe(bool reuse_id) : reuse_id_(reuse_id) {}

    void on_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                strategy_entry("S", false, kNaN, kNaN, 2.0);
                strategy_exit("X", "S", 80.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", 2.0);
                break;
            case 2: strategy_close_all(); break;
            case 4: strategy_entry(reuse_id_ ? "S" : "S2", false, kNaN, kNaN,
                                   2.0); break;
            case 7: strategy_close_all(); break;
            default: break;
        }
    }

private:
    bool reuse_id_;
};

static std::vector<Bar> stale_leg_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0  entry + the X bracket @80
        mk(100, 101,  99, 100, 2000),   // 1  S fills @100
        mk(100, 101,  99, 100, 3000),   // 2  close_all signalled
        mk(100, 101,  99, 100, 4000),   // 3  it fills @100 -> FLAT
        mk(100, 101,  99, 100, 5000),   // 4  cycle-2 entry signalled
        mk(100, 101,  99, 100, 6000),   // 5  it fills @100
        mk(100, 101,  79,  80, 7000),   // 6  80 is touched — X must NOT fire
        // The close_all settles at 85, clear of the stale leg's 80, so a row
        // priced at 80 can only mean X fired.
        mk( 85,  86,  84,  85, 8000),   // 7  close_all signalled
        mk( 85,  86,  84,  85, 9000),   // 8  it fills @85
        mk( 85,  86,  84,  85, 10000),  // 9
    };
}

}  // namespace

// ---- A: the orphaned bucket's leg still fires ------------------------------

static void test_orphaned_bucket_leg_still_fires() {
    std::printf("test_orphaned_bucket_leg_still_fires\n");
    TwoBucketProbe eng;
    auto bars = two_bucket_bars();
    eng.run(bars.data(), (int)bars.size());

    // 4 units in, 4 units out on the two bracket bars, then the 06-18 cycle.
    CHECK(eng.trade_count() == 5);
    if (eng.trade_count() < 4) return;

    // T1 pair: both units come from the S bucket (position-level FIFO), and
    // AT1 — whose from_entry is SA — legitimately takes one of them. That
    // cross-bucket FILL already worked; it is the label TV shows as "Add T1".
    CHECK(eng.exit_id(0) == std::string("T1"));
    CHECK(eng.entry_id(0) == std::string("S"));
    CHECK(near(eng.exit_price(0), 96.0));
    CHECK(eng.exit_id(1) == std::string("AT1"));
    CHECK(eng.entry_id(1) == std::string("S"));
    CHECK(near(eng.exit_price(1), 96.0));

    // T2 pair. THE FIX: "T2" is bound to entry id S, whose bucket was fully
    // drained on bar 2 — pre-fix it was Removed and this row did not exist.
    // The unit it takes is an SA unit: TV's cross-assigned "T2 Exit" on a
    // ▼+ ADD entry (#129).
    CHECK(eng.exit_id(2) == std::string("T2"));
    CHECK(eng.entry_id(2) == std::string("SA"));
    CHECK(near(eng.exit_price(2), 90.0));
    CHECK(eng.exit_id(3) == std::string("AT2"));
    CHECK(eng.entry_id(3) == std::string("SA"));
    CHECK(near(eng.exit_price(3), 90.0));

    CHECK(eng.rows_with_exit_id("T2") == 1);   // the orphaned leg fired
    CHECK(eng.rows_at_exit_price(96.0) == 2);
    CHECK(eng.rows_at_exit_price(90.0) == 2);
    for (int i = 0; i < 4; ++i) CHECK(near(eng.size(i), 1.0));
}

// ---- B: reaching flat re-admits the next cycle's same-id entry -------------

static void test_flat_readmits_next_cycle_entry() {
    std::printf("test_flat_readmits_next_cycle_entry\n");
    TwoBucketProbe eng;
    auto bars = two_bucket_bars();
    eng.run(bars.data(), (int)bars.size());

    // Pre-fix the engine carried a phantom unit and never went flat, so the
    // monotone pyramiding counter (p1) legitimately refused this entry — for a
    // position TV does not have. With the leg lifetime fixed the engine is
    // flat on bar 3, the counter resets, and the entry is admitted.
    CHECK(eng.trade_count() == 5);
    if (eng.trade_count() < 5) return;
    CHECK(eng.entry_id(4) == std::string("S"));
    CHECK(near(eng.size(4), 2.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- C: the gate still has teeth ------------------------------------------

static void test_leg_for_unfilled_entry_id_is_removed() {
    std::printf("test_leg_for_unfilled_entry_id_is_removed\n");
    GhostLegProbe eng;
    auto bars = two_bucket_bars();
    eng.run(bars.data(), (int)bars.size());

    // GHOST's limit (96) is touched on bar 2, but "NEVER" never filled in this
    // position cycle, so the leg is Removed and only REAL closes the position.
    CHECK(eng.rows_with_exit_id("GHOST") == 0);
    CHECK(eng.rows_at_exit_price(96.0) == 0);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() < 1) return;
    CHECK(eng.exit_id(0) == std::string("REAL"));
    CHECK(near(eng.size(0), 2.0));
    CHECK(near(eng.exit_price(0), 90.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- D: a prior cycle's leg cannot fire against the new position -----------

static void test_stale_leg_does_not_fire_in_next_cycle() {
    std::printf("test_stale_leg_does_not_fire_in_next_cycle\n");
    auto bars = stale_leg_bars();

    // Different entry id in cycle 2: cycle_filled_entry_ids_ was cleared at
    // flat and now holds only "S2", so the "S"-bound leg is Removed.
    StaleLegProbe fresh_id(/*reuse_id=*/false);
    fresh_id.run(bars.data(), (int)bars.size());
    CHECK(fresh_id.rows_with_exit_id("X") == 0);
    CHECK(fresh_id.rows_at_exit_price(80.0) == 0);
    CHECK(fresh_id.trade_count() == 2);
    CHECK(near(fresh_id.position_size(), 0.0));

    // Same entry id in cycle 2 — the harder case: provenance alone cannot
    // distinguish the cycles, so this one is held by the full-close purge.
    StaleLegProbe same_id(/*reuse_id=*/true);
    same_id.run(bars.data(), (int)bars.size());
    CHECK(same_id.rows_with_exit_id("X") == 0);
    CHECK(same_id.rows_at_exit_price(80.0) == 0);
    CHECK(same_id.trade_count() == 2);
    CHECK(near(same_id.position_size(), 0.0));
}

// ---- rerun determinism -----------------------------------------------------

static void test_rerun_reproduces_the_cycle_set() {
    std::printf("test_rerun_reproduces_the_cycle_set\n");
    TwoBucketProbe eng;
    auto bars = two_bucket_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 5);

    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 5);
    CHECK(eng.rows_with_exit_id("T2") == 1);
    CHECK(eng.rows_at_exit_price(90.0) == 2);
    CHECK(near(eng.position_size(), 0.0));
}

int main() {
    std::printf("=== test_exit_bracket_position_cycle_lifetime ===\n");

    test_orphaned_bucket_leg_still_fires();
    test_flat_readmits_next_cycle_entry();
    test_leg_for_unfilled_entry_id_is_removed();
    test_stale_leg_does_not_fire_in_next_cycle();
    test_rerun_reproduces_the_cycle_set();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
