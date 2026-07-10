/*
 * test_close_all_coqueued_entry.cpp — M1v2 (NARROWED close-co-queue fix).
 *
 * After a DEFERRED full-close exit fills and flattens on a bar, a pending
 * SAME-direction MARKET/ENTRY order co-queued on the close's own call bar is
 * REMOVED iff it is a prior-bar carry (created_bar != exit_closed_from_bar) OR
 * it was OVER the pyramiding cap at placement. A co-queued same-direction entry
 * that was WITHIN the cap at placement SURVIVES (TradingView keeps it: a market
 * fills at the next open; a stop fires when its level is later touched).
 *
 * Why the extra "over cap" term (vs the reverted M1, which used created_bar
 * alone): the engine enforces pyramiding at FILL time, and the co-queued full
 * close zeroes position_entry_count_ before the add fills, so the fill-time gate
 * passes an add TradingView would have rejected at placement. The post-full-
 * close wipe is the only site that catches those. M1's created_bar-only
 * exemption un-cancelled over-cap adds → probe65 doubled (732→1463) and the
 * composite bracket fell below strong. The narrowed rule snapshots the
 * placement-time over-cap status on the PendingOrder and keeps only genuinely
 * TV-admissible (within-cap) co-queues.
 *
 * Ground truth:
 *   - corpus/validation/pyramid-deferred-flip-close-all-01 (pyramiding=4):
 *     9 TV-only entries 0-30min after a 21:45 close_all, all UNDER cap
 *     (event replay: max same-dir open = 2 < 4) → must survive (R-KEEP).
 *   - corpus/validation/order-same-id-entry-close-same-bar-01 (pyramiding=1):
 *     over-cap same-id add + close(id) co-queued → dropped 366/366 (G-DROP).
 *   - corpus/validation/composite-bracket-cap-range-pending-stop-01
 *     (pyramiding=1): over-cap strategy.entry(stop) re-armed on the bar a full
 *     strategy.order exit flattens → dropped (G-DROP).
 *   See data/progress/laneb-pyramid-closeall-diagnosis.md and the session
 *   scratchpad m1-regression-diagnosis.md.
 *
 * NON-POOC harness: process_orders_on_close_ stays false, so close_all/close
 * are DEFERRED market exits filling at the next bar's open (the probe's
 * 21:45-call / 22:00-fill split), which is the code path carrying the bug.
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

// Common probe base: fixed 1-lot sizing, no slippage/commission, tick 0.01.
class ProbeBase : public BacktestEngine {
public:
    explicit ProbeBase(int pyr) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = pyr;
        syminfo_mintick_ = 0.01;
    }
    double pos_size() const { return signed_position_size(); }
};

// ─────────────────────────────────────────────────────────────────────
// R-KEEP-mkt (pyramiding=4, HEADROOM: TWO same-dir opens).
// A market entry co-queued with close_all on the close's own call bar, while
// UNDER the pyramiding cap, must survive and open its leg. This is the pyramid
// probe's KEEP flavor with genuine headroom (2 open < cap 4) so it discriminates
// from the over-cap DROP cases below — the reverted M1 test only ever exercised
// pyramiding=2 with ONE open, which never distinguished the two.
//
//   bar0: entry("L0", mkt)
//   bar1: L0 fills @100 → LONG 1. entry("L1", mkt)
//   bar2: L1 fills @100 → LONG 2 (count 2). entry("L2", mkt) [3rd, UNDER cap 4]
//         + close_all()  → both created_bar 2
//   bar3: deferred close_all fills @100 → FLAT (L0,L1 closed). L2 (created_bar 2
//         == exit_closed_from_bar 2, within cap) survives → fills @100 → LONG 1.
//
// EXPECTED (fixed): position ends LONG 1. Pre-fix: L2 wiped → FLAT.
// ─────────────────────────────────────────────────────────────────────
static void test_R_KEEP_mkt_undercap_survives() {
    std::printf("R-KEEP-mkt (pyr=4, under-cap market co-queue survives)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L0", true);
            if (bar_index_ == 1) strategy_entry("L1", true);
            if (bar_index_ == 2) {
                strategy_entry("L2", true, kNaN, kNaN, kNaN, "under-cap add");
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: L0 fills
        mk(100, 100, 100, 100, 1'800'000),   // bar2: L1 fills; queue L2 + close_all
        mk(100, 100, 100, 100, 2'400'000),   // bar3: close_all fills; L2 must fill
        mk(100, 100, 100, 100, 3'000'000),   // bar4: settle
    };
    p.run(bars, 5);
    CHECK(near(p.pos_size(), 1.0));           // pre-fix: 0.0 (L2 wrongly cancelled)
}

// ─────────────────────────────────────────────────────────────────────
// R-KEEP-stop (pyramiding=4, HEADROOM, SHORT). A same-direction stop entry
// co-queued with close_all while UNDER cap survives and fills when its level is
// later touched (the probe's 04-27 22:00 short-stop @1793.76 flavor).
//
//   bar0: entry("S0", short mkt)
//   bar1: S0 fills @100 → SHORT 1. entry("S1", short mkt)
//   bar2: S1 fills @100 → SHORT 2 (count 2). arm short stop "SS"@90 [3rd, UNDER
//         cap 4] + close_all()  → both created_bar 2
//   bar3: deferred close_all fills @100 → FLAT. SS survives (low 99 > 90).
//   bar4: low 88 ≤ 90 → SS fires → SHORT 1.
//
// EXPECTED (fixed): position ends SHORT 1. Pre-fix: SS wiped → FLAT.
// ─────────────────────────────────────────────────────────────────────
static void test_R_KEEP_stop_undercap_survives() {
    std::printf("R-KEEP-stop (pyr=4, under-cap short stop co-queue survives)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S0", false);
            if (bar_index_ == 1) strategy_entry("S1", false);
            if (bar_index_ == 2) {
                strategy_entry("SS", false, kNaN, /*stop=*/90.0, kNaN, "under-cap short stop");
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: S0 fills
        mk(100, 102,  99, 100, 1'800'000),   // bar2: S1 fills; arm SS@90 + close_all
        mk(100, 102,  99, 100, 2'400'000),   // bar3: close_all fills; SS survives, untouched
        mk( 95,  95,  88,  90, 3'000'000),   // bar4: low 88 ≤ 90 → SS fires
        mk( 90,  91,  89,  90, 3'600'000),   // bar5: settle
    };
    p.run(bars, 6);
    CHECK(near(p.pos_size(), -1.0));          // pre-fix: 0.0 (SS wrongly cancelled)
}

// ─────────────────────────────────────────────────────────────────────
// G-DROP-mkt (probe65, pyramiding=1). An OVER-cap same-id market add co-queued
// with a deferred close(id) on the close's own call bar is REMOVED — TradingView
// never admits it (add-drop 366/366). Passes pre- AND post-fix (this is the pin
// the reverted M1 regressed).
//
//   bar0: entry("L", mkt)
//   bar1: L fills @100 → LONG 1 (count 1). entry("L", mkt) [re-place, OVER cap:
//         count 1 ≥ pyr 1] + close("L")  → both created_bar 1
//   bar2: deferred close("L") fills @100 → FLAT (1 trade). The add (created_bar 1
//         == exit_closed_from_bar 1 BUT over_pyramiding_cap_at_placement) is
//         REMOVED → position stays FLAT.
//
// EXPECTED (pre- and post-fix): 1 trade, position ends FLAT.
// ─────────────────────────────────────────────────────────────────────
static void test_G_DROP_mkt_overcap_probe65() {
    std::printf("G-DROP-mkt (probe65, pyr=1, over-cap same-id add dropped)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(1) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 1.0, "open long");
            if (bar_index_ == 1) {
                strategy_entry("L", true, kNaN, kNaN, 1.0, "same-pass add long");
                strategy_close("L", "same-pass close long");
            }
        }
    };
    Probe p;
    Bar bars[4] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: L fills; re-place L + close L
        mk(100, 100, 100, 100, 1'800'000),   // bar2: close fills; add dropped
        mk(100, 100, 100, 100, 2'400'000),   // bar3: settle
    };
    p.run(bars, 4);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));           // over-cap add dropped → FLAT (pre+post)
}

// ─────────────────────────────────────────────────────────────────────
// G-DROP-stop (bracket, pyramiding=1). An OVER-cap same-direction stop entry
// (strategy.entry with stop) re-armed on the bar a full strategy.order exit
// flattens the position is REMOVED. This is the composite-bracket shape:
// LongOnGap re-arms every bar; on the bar BracketSL (a full strategy.order
// opposite-side exit) flattens, the re-armed stop must not survive to open a
// phantom leg. Passes pre- AND post-fix.
//
//   bar0: entry("L0", mkt)
//   bar1: L0 fills @100 → LONG 1 (count 1). arm long stop "LG"@200 [OVER cap:
//         count 1 ≥ pyr 1] + strategy.order("X", short, qty=1) [full RAW market
//         exit]  → both created_bar 1
//   bar2: X fills @100 → FLAT (1 trade), exit_closed_from_bar = 1. LG (created_bar
//         1 == 1 BUT over cap) REMOVED. High 150 < 200 → LG not touched anyway.
//   bar3: high 250 ≥ 200 — LG would fill here if it had survived; it must NOT.
//
// EXPECTED (pre- and post-fix): 1 trade, position ends FLAT.
// ─────────────────────────────────────────────────────────────────────
static void test_G_DROP_stop_overcap_bracket() {
    std::printf("G-DROP-stop (bracket, pyr=1, over-cap re-armed stop dropped)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(1) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L0", true);
            if (bar_index_ == 1) {
                strategy_entry("LG", true, kNaN, /*stop=*/200.0, kNaN, "re-armed long stop");
                strategy_order("X", false, /*qty=*/1.0);   // full RAW market exit (opp dir)
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: L0 fills; arm LG@200 + RAW exit X
        mk(100, 150,  99, 100, 1'800'000),   // bar2: X flattens; LG dropped (high 150<200)
        mk(210, 250, 210, 240, 2'400'000),   // bar3: high 250≥200 — LG would fill if alive
        mk(240, 240, 240, 240, 3'000'000),   // bar4: settle
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));           // over-cap re-arm dropped → FLAT (pre+post)
}

// ─────────────────────────────────────────────────────────────────────
// G-DROP-mkt-pyr2 (pyramiding=2, AT cap). A market add co-queued with close_all
// while AT the cap (2 open, pyr 2) is REMOVED — over cap at placement even though
// the co-queued close zeroes the count before the add would fill. This is the row
// a created_bar-only exemption gets wrong (it would KEEP the add); the narrowed
// rule's over-cap term drops it. Passes pre- AND post-fix.
//
//   bar0: entry("L0", mkt)
//   bar1: L0 fills @100 → LONG 1. entry("L1", mkt)
//   bar2: L1 fills @100 → LONG 2 (count 2 == cap). entry("L2", mkt) [OVER cap]
//         + close_all()  → both created_bar 2
//   bar3: close_all fills → FLAT. L2 (created_bar 2 == 2 BUT over cap) REMOVED.
//
// EXPECTED (pre- and post-fix): position ends FLAT.
// ─────────────────────────────────────────────────────────────────────
static void test_G_DROP_mkt_overcap_pyr2() {
    std::printf("G-DROP-mkt-pyr2 (pyr=2, at-cap market co-queue dropped)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(2) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L0", true);
            if (bar_index_ == 1) strategy_entry("L1", true);
            if (bar_index_ == 2) {
                strategy_entry("L2", true, kNaN, kNaN, kNaN, "over-cap add");
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: L0 fills
        mk(100, 100, 100, 100, 1'800'000),   // bar2: L1 fills (LONG 2); queue L2 + close_all
        mk(100, 100, 100, 100, 2'400'000),   // bar3: close_all fills; L2 dropped
        mk(100, 100, 100, 100, 3'000'000),   // bar4: settle
    };
    p.run(bars, 5);
    CHECK(near(p.pos_size(), 0.0));           // over-cap add dropped → FLAT (pre+post)
}

// ─────────────────────────────────────────────────────────────────────
// G-carry (prior-bar carry still cancelled). An entry created on a bar BEFORE
// the close_all call bar is NOT co-queued (created_bar != exit_closed_from_bar)
// and must STILL be cancelled — preserving the deferred-flip carry semantics the
// wipe exists for (probes 72/80/93). Passes pre- AND post-fix.
//
//   bar0: entry("L0", mkt)
//   bar1: L0 fills @100 → LONG 1. arm long stop "LC"@120 (created_bar 1) — a
//         carry, placed a bar BEFORE the close call.
//   bar2: close_all() (created_bar 2, deferred). LC pending.
//   bar3: close_all fills @100 → FLAT (1 trade). exit_closed_from_bar = 2.
//         LC (created_bar 1 != 2) → REMOVED.
//   bar4: high 130 ≥ 120 — LC would fill here if it had survived; it must NOT.
//
// EXPECTED (pre- and post-fix): 1 trade, position ends FLAT.
// ─────────────────────────────────────────────────────────────────────
static void test_G_carry_priorbar_still_cancelled() {
    std::printf("G-carry (prior-bar carry still cancelled)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(2) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L0", true);
            if (bar_index_ == 1)
                strategy_entry("LC", true, kNaN, /*stop=*/120.0, kNaN, "prior-bar carry");
            if (bar_index_ == 2) strategy_close_all();
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: L0 fills; arm LC@120 (carry)
        mk(100, 100, 100, 100, 1'800'000),   // bar2: close_all() called
        mk(100, 100, 100, 100, 2'400'000),   // bar3: close_all fills → FLAT; LC removed
        mk(125, 130, 125, 128, 3'000'000),   // bar4: high 130 ≥ 120 — LC would fill if alive
        mk(128, 128, 128, 128, 3'600'000),   // bar5: settle
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));           // carry cancelled → FLAT (pre+post)
}

// ─────────────────────────────────────────────────────────────────────
// G-opposite (KI-64 untouched — characterization). An opposite-direction entry
// co-queued with close_all is never a target of the same-direction wipe
// (is_long != exit_closed_was_long); its behavior is IDENTICAL before and after
// this fix. Pins the current engine behavior: the opposite short stop survives
// and fills when touched.
//
//   bar0: entry("L0", mkt)
//   bar1: L0 fills @100 → LONG 1. arm OPPOSITE short stop "SOPP"@90 + close_all()
//   bar2: close_all fills @100 → FLAT (1 trade). SOPP opposite dir → untouched;
//         low 99 > 90 stays pending.
//   bar3: low 88 ≤ 90 → SOPP fires → SHORT 1.
//
// EXPECTED (pre- and post-fix, characterization): position ends SHORT 1.
// ─────────────────────────────────────────────────────────────────────
static void test_G_opposite_unchanged_ki64() {
    std::printf("G-opposite (KI-64 opposite-direction unchanged)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(2) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L0", true);
            if (bar_index_ == 1) {
                strategy_entry("SOPP", false, kNaN, /*stop=*/90.0, kNaN, "opposite short stop");
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),   // bar0
        mk(100, 100, 100, 100, 1'200'000),   // bar1: L0 fills; arm SOPP@90 + close_all
        mk(100, 102,  99, 100, 1'800'000),   // bar2: close_all fills; SOPP untouched
        mk( 95,  95,  88,  90, 2'400'000),   // bar3: low 88 ≤ 90 → SOPP fires
        mk( 90,  91,  89,  90, 3'000'000),   // bar4: settle
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), -1.0));          // opposite entry survives (unchanged pre/post)
}

int main() {
    test_R_KEEP_mkt_undercap_survives();
    test_R_KEEP_stop_undercap_survives();
    test_G_DROP_mkt_overcap_probe65();
    test_G_DROP_stop_overcap_bracket();
    test_G_DROP_mkt_overcap_pyr2();
    test_G_carry_priorbar_still_cancelled();
    test_G_opposite_unchanged_ki64();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
