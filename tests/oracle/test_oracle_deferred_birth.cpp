/*
 * R4-D L0 literal legacy-route oracle.
 * Captured at ab9714beccb62b796c122cf68986ec9e7dbf4a67.
 * Expected values below are frozen from the current source::PineStrategyHost /
 * LegacyCompatibilityConsumer route.  Do not derive or update them from a
 * native lowering implementation.
 */

/*
 * test_exit_bracket_pending_entry_leg.cpp — TV binds exit brackets to ENTRY
 * INSTANCES via from_entry, not to the net position.
 *
 * A strategy.exit re-issue MODIFIES every live leg carrying that exit id (each
 * keeping its own binding) and ADDITIONALLY arms one new leg bound to the
 * pending entry order whose id == from_entry, if one exists at that moment.
 * The engine armed exactly ONE order per exit id, so a re-issue that coincided
 * with a pending same-id entry under-reserved: two qty=1 brackets covered only
 * 2 units of a position that became 4.
 *
 * The paired-bracket probe's rule, illustrated synthetically,
 * pyramiding=2, ETH-USDT-USDT 15m. The three shapes below must
 * hold SIMULTANEOUSLY; each one falsifies a different naive rule.
 *
 * (i)  LEG MULTIPLICITY. A 2u long carried from a prior bar with both
 *      LongT1/LongT2 live; an OR bar re-issues both brackets (stop
 *      re-priced to orLow=1410.00) while a same-id 2u entry is pending.
 *      The stop closes FOUR units at 1410.00 later, tagged T1/T2/T1/T2 —
 *      a full bracket pair PER ENTRY INSTANCE, carried pair first (FIFO):
 *          T1 1410.00 q1  carried entry @1400.00
 *          T2 1410.00 q1  carried entry @1400.00
 *          T1 1410.00 q1  added entry   @1420.00
 *          T2 1410.00 q1  added entry   @1420.00
 *      Engine pre-fix: only the carried pair closed; the added 2u collapsed
 *      into one unprotected trade that survived to a later reversal.
 *
 * (ii) THE COUNTER-CASE that refutes a blanket multiply. Same
 *      shape (live carried Short bracket + pending same-id entry, both
 *      brackets re-issued in the same block) but ShortT1 had already been
 *      CONSUMED earlier, so it has no live leg and arms only the ONE
 *      pending-entry leg. A single T1 later closes exactly 1 unit
 *      (the carried remnant, FIFO). `reserved = qty * (open legs + pending
 *      entries)` would close 2 units at 1422.00 and desync later trades —
 *      a regression on a locus the engine already matches. Green both pre- and
 *      post-fix by construction: it is the guard, not the repro.
 *
 * (iii) NO ADMISSIBLE PENDING ENTRY. The third short is over
 *      pyramiding=2 and TV rejects it, so the re-issue binds to nothing new:
 *      ShortT1 (no live leg, no admissible pending entry) arms NOTHING because
 *      the two live ShortT2 legs already reserve the whole position, and
 *      ShortT2 re-arms BOTH its legs. A later stop @1426.00 closes 2 units
 *      tagged "T2 Exit" / "T2 Exit" — the double-T2 label that only this model
 *      reproduces. Needs the pyramiding-count fix (an entry the cap will
 *      refuse at fill contributes no bracket leg). Assertions retain original inputs.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
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

class BracketProbe : public pineforge::source::PineStrategyHost {
public:
    BracketProbe() {
        initial_capital_ = 1000000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        pyramiding_ = 2;
        process_orders_on_close_ = false;
    }

    std::string exit_id(int i) const { return closed_trade_exit_id(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
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

protected:
    // The strategy's bracket pair: qty=1 T1/T2 legs sharing one stop, both
    // attached to entry id "L" — the paired-bracket probe's shape.
    void arm_brackets(double t1_limit, double t2_limit, double stop) {
        strategy_exit("T1", "L", t1_limit, stop, kNaN, kNaN, kNaN,
                      100.0, "", 1.0);
        strategy_exit("T2", "L", t2_limit, stop, kNaN, kNaN, kNaN,
                      100.0, "", 1.0);
    }
};

// ── (i) A re-issue over a pending same-id entry arms both legs ─────────────
//
//  bar 0  entry L(2u) + T1/T2 (stop 90)          [flat: one deferred leg each]
//  bar 1  L fills @100                            pos 2u  [L:2]
//  bar 2  entry L(2u) AGAIN + T1/T2 re-issued (stop 95) while L is pending
//         -> T1: 1 live leg + 1 pending entry = 2 legs
//         -> T2: 1 live leg + 1 pending entry = 2 legs
//  bar 3  L#2 fills @110                          pos 4u  [L:2 @100, L:2 @110]
//  bar 4  low 90 crosses the 95 stop -> ALL FOUR legs fire @95
class CarriedPairProbe : public BracketProbe {
public:
    void on_source_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(200.0, 300.0, 90.0);
                break;
            case 2:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(210.0, 310.0, 95.0);
                break;
            default: break;
        }
    }
};

static std::vector<Bar> carried_pair_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0
        mk(100, 101,  99, 100, 2000),   // 1  L fills @100
        mk(100, 101,  99, 100, 3000),   // 2  re-issue over the pending L
        mk(110, 111, 105, 110, 4000),   // 3  L#2 fills @110
        mk(105, 106,  90,  95, 5000),   // 4  stop 95 crossed
        mk( 95,  96,  94,  95, 6000),   // 5
    };
}

// ── (ii)+(iii): consumed leg, then a cap-refused entry ─────────────────────
//
//  bar 0  entry L(2u) + T1(lim 110)/T2(lim 150), stop 80
//  bar 1  L fills @100                            pos 2u  [A:2]
//  bar 2  high 111 -> T1 limit 110 fills 1u       pos 1u  [A:1]   T1 CONSUMED
//  bar 3  entry L(2u) + T1(lim 105)/T2(lim 150) re-issued while L is pending
//         -> T1: 0 live legs + 1 pending entry = ONE leg      <- (ii)
//         -> T2: 1 live leg  + 1 pending entry = two legs
//  bar 4  L#2 fills @100                          pos 3u  [A:1, B:2]  count 2
//  bar 5  high 106 -> T1 limit 105 fills exactly 1u (FIFO -> A's remnant)
//         pos 2u [B:2]; entry count stays 2 (monotone)
//         then: entry L(2u) + T1(lim 200)/T2(lim 250) re-issued, stop 95
//         -> the pending L is over pyramiding=2: no admissible pending entry
//         -> T1: 0 live legs, and the two live T2 legs already reserve the
//                whole 2u position -> NOTHING armed                <- (iii)
//         -> T2: 2 live legs, no pending entry -> BOTH re-armed
//  bar 6  L#3's fill attempt is refused by the pyramiding cap  pos 2u
//  bar 7  low 90 crosses the 95 stop -> 2 units @95, BOTH tagged T2
class ConsumedLegProbe : public BracketProbe {
public:
    void on_source_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(110.0, 150.0, 80.0);
                break;
            case 3:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(105.0, 150.0, 80.0);
                break;
            case 5:
                strategy_entry("L", true, kNaN, kNaN, 2.0);
                arm_brackets(200.0, 250.0, 95.0);
                break;
            default: break;
        }
    }
};

static std::vector<Bar> consumed_leg_bars() {
    return {
        mk(100, 101,  99, 100, 1000),   // 0
        mk(100, 101,  99, 100, 2000),   // 1  L fills @100
        mk(100, 111,  99, 100, 3000),   // 2  T1 limit 110 -> 1u
        mk(100, 101,  99, 100, 4000),   // 3  re-issue over the pending L
        mk(100, 101,  99, 100, 5000),   // 4  L#2 fills @100
        mk(100, 106,  99, 100, 6000),   // 5  T1 limit 105 -> exactly 1u
        mk(100, 101,  99, 100, 7000),   // 6  L#3 refused by the cap
        mk(100, 101,  90,  95, 8000),   // 7  stop 95 crossed
        mk( 95,  96,  94,  95, 9000),   // 8
    };
}

}  // namespace

// ---- (i) four units exit at the stop, a full pair per entry instance -------

static void test_reissue_over_pending_entry_arms_a_pair_per_instance() {
    std::printf("test_reissue_over_pending_entry_arms_a_pair_per_instance\n");
    CarriedPairProbe eng;
    auto bars = carried_pair_bars();
    eng.run(bars.data(), (int)bars.size());

    // TV's row ordering: T1/T2 against the carried lot (FIFO first), then T1/T2
    // against the added lot. Pre-fix only the first two rows existed and 2
    // units survived unprotected.
    CHECK(eng.trade_count() == 4);
    CHECK(eng.rows_with_exit_id("T1") == 2);
    CHECK(eng.rows_with_exit_id("T2") == 2);
    for (int i = 0; i < eng.trade_count() && i < 4; ++i) {
        CHECK(near(eng.size(i), 1.0));
        CHECK(near(eng.exit_price(i), 95.0));
    }
    if (eng.trade_count() == 4) {
        CHECK(eng.exit_id(0) == std::string("T1"));
        CHECK(near(eng.entry_price(0), 100.0));   // carried lot
        CHECK(eng.exit_id(1) == std::string("T2"));
        CHECK(near(eng.entry_price(1), 100.0));   // carried lot
        CHECK(eng.exit_id(2) == std::string("T1"));
        CHECK(near(eng.entry_price(2), 110.0));   // added lot
        CHECK(eng.exit_id(3) == std::string("T2"));
        CHECK(near(eng.entry_price(3), 110.0));   // added lot
    }
    CHECK(near(eng.position_size(), 0.0));        // pre-fix: 2.0 survived
}

// ---- (ii) a consumed leg arms ONE leg; the limit closes exactly 1 unit -----
// ---- (iii) a cap-refused entry arms no new leg; both T2 legs re-price ------

static void test_consumed_leg_and_capped_entry() {
    std::printf("test_consumed_leg_and_capped_entry\n");
    ConsumedLegProbe eng;
    auto bars = consumed_leg_bars();
    eng.run(bars.data(), (int)bars.size());

    // rows: T1@110 (1u), T1@105 (1u), then the 2-unit stop-out @95.
    CHECK(eng.trade_count() == 4);
    if (eng.trade_count() < 4) return;

    // (ii) the 2026-03-27 locus. ShortT1 had no live leg, so the re-issue arms
    // exactly ONE leg and the limit touch closes exactly ONE unit — FIFO
    // against the carried remnant. A blanket qty*(legs+pending) multiply would
    // close 2 here.
    CHECK(eng.exit_id(0) == std::string("T1"));
    CHECK(near(eng.size(0), 1.0));
    CHECK(near(eng.exit_price(0), 110.0));
    CHECK(eng.exit_id(1) == std::string("T1"));
    CHECK(near(eng.size(1), 1.0));
    CHECK(near(eng.exit_price(1), 105.0));
    CHECK(near(eng.entry_price(1), 100.0));       // the carried remnant, FIFO

    // (iii) the 2026-03-29 locus. The over-cap entry never fills, so no third
    // lot appears; T1 arms nothing and BOTH surviving T2 legs fire at the stop.
    CHECK(eng.exit_id(2) == std::string("T2"));
    CHECK(near(eng.size(2), 1.0));
    CHECK(near(eng.exit_price(2), 95.0));
    CHECK(eng.exit_id(3) == std::string("T2"));
    CHECK(near(eng.size(3), 1.0));
    CHECK(near(eng.exit_price(3), 95.0));
    CHECK(eng.rows_with_exit_id("T1") == 2);      // never a third T1
    CHECK(eng.rows_with_exit_id("T2") == 2);
    CHECK(near(eng.position_size(), 0.0));
}

// ---- rerun determinism (handle reuse) --------------------------------------

static void test_rerun_reproduces_the_leg_census() {
    std::printf("test_rerun_reproduces_the_leg_census\n");
    CarriedPairProbe eng;
    auto bars = carried_pair_bars();
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 4);

    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 4);
    CHECK(eng.rows_with_exit_id("T1") == 2);
    CHECK(eng.rows_with_exit_id("T2") == 2);
    CHECK(near(eng.position_size(), 0.0));
}

int main() {
    std::printf("=== test_exit_bracket_pending_entry_leg ===\n");

    test_reissue_over_pending_entry_arms_a_pair_per_instance();
    test_consumed_leg_and_capped_entry();
    test_rerun_reproduces_the_leg_census();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
