/*
 * test_close_all_coqueued_entry.cpp — M1v2 (NARROWED close-co-queue fix).
 *
 * After a DEFERRED full-close exit fills and flattens on a bar, the ordinary
 * stale same-direction MARKET/ENTRY wipe keeps three independently pinned
 * exceptions: an under-cap same-call-bar co-queue, a resting prior-bar pure
 * LIMIT, and (for close_all only) a prior-bar under-cap pure STOP whose id still
 * names a physically-live same-side lot when close_all is called. Everything
 * else — different-id carries, stop-limits, close(id), and over-cap adds —
 * remains in the wipe.
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

// The provenance identity is carried by all three broker schedulers. Keep one
// compact probe that can run through ordinary COOF bars or real lower-TF
// magnifier endpoints, and can replace the deferred close_all with a RAW order
// using its synthetic id. Fill-recalc bodies are deliberately inert: these
// controls isolate broker-order provenance rather than script re-emission.
class CoofIncarnationProbe final : public ProbeBase {
public:
    explicit CoofIncarnationProbe(bool replace_close_all)
        : ProbeBase(4), replace_close_all_(replace_close_all) {
        calc_on_order_fills_ = true;
    }

    void on_bar(const Bar&) override {
        if (coof_fill_recalc_active_) return;
        if (bar_index_ == 0) strategy_entry("S", false);
        if (bar_index_ == 1)
            strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                           "prior-bar same-id stop");
        if (bar_index_ == 2) {
            strategy_close_all();
            if (replace_close_all_)
                strategy_order("__close__", true, /*qty=*/1.0);
        }
    }

private:
    bool replace_close_all_;
};

static void run_coof_incarnation_control(bool replace_close_all,
                                         bool magnifier) {
    CoofIncarnationProbe p(replace_close_all);
    if (!magnifier) {
        Bar bars[5] = {
            mk(100, 100, 100, 100,   600'000),
            mk(100, 102,  99, 100, 1'200'000),
            mk(100, 102,  99, 100, 1'800'000),
            mk(100, 102,  88,  90, 2'400'000),
            mk( 90,  91,  89,  90, 3'000'000),
        };
        p.run(bars, 5);
    } else {
        // Two 1-minute bars compose each 2-minute chart bar. On chart bar 3,
        // the close/RAW fills at the first lower-bar open and the STOP is only
        // reached by a later lower-bar endpoint.
        Bar lower[10] = {
            mk(100, 100, 100, 100,       0),
            mk(100, 100, 100, 100,  60'000),
            mk(100, 101,  99, 100, 120'000),
            mk(100, 102,  99, 100, 180'000),
            mk(100, 101,  99, 100, 240'000),
            mk(100, 102,  99, 100, 300'000),
            mk(100, 102,  99, 100, 360'000),
            mk(100, 100,  88,  90, 420'000),
            mk( 90,  91,  89,  90, 480'000),
            mk( 90,  91,  89,  90, 540'000),
        };
        p.run(lower, 10, "1", "2", /*bar_magnifier=*/true,
              /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);
    }

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), replace_close_all ? 0.0 : -1.0));
}

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
// later touched. This pins the existing same-call-bar rule; the exact 04-27
// prior-bar/same-ID shape is pinned separately below.
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

// ─────────────────────────────────────────────────────────────────────
// R-KEEP-prior-stop-fill-bar (the exact pyramid probe shape). A pure STOP
// strategy.entry reusing the physically-live entry id was armed one bar BEFORE
// close_all was called. TradingView keeps this broker order: when close_all
// fills at the next open and the same bar subsequently reaches the stop, the
// old short closes and the pending same-id short opens.
//
// This pins classify_order_eligibility: the STOP is reached later in the same
// pending-order pass after the deferred close_all has set exit_closed_from_bar.
// ─────────────────────────────────────────────────────────────────────
static void test_R_KEEP_priorbar_same_id_stop_touched_on_close_fill_bar() {
    std::printf("R-KEEP-prior-stop-fill-bar (physical same-id STOP survives close_all)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) strategy_close_all();
        }
    };
    Probe p;
    Bar bars[5] = {
        mk(100, 100, 100, 100,   600'000),   // bar0: queue physical S
        mk(100, 102,  99, 100, 1'200'000),   // bar1: S fills; arm pending S@90
        mk(100, 102,  99, 100, 1'800'000),   // bar2: deferred close_all call
        mk(100, 102,  88,  90, 2'400'000),   // bar3: close fills, then S@90 fires
        mk( 90,  91,  89,  90, 3'000'000),   // bar4: settle
    };
    p.run(bars, 5);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), -1.0));          // pre-fix: 0.0 (eligibility removes S)
}

// ─────────────────────────────────────────────────────────────────────
// R-KEEP-prior-stop-later (same provenance, but the close-fill bar does NOT
// touch the stop). The STOP must survive end-of-pass compaction and fill on a
// later bar. This is deliberately separate from the prior test so a patch to
// only one of the two cleanup sites cannot pass.
// ─────────────────────────────────────────────────────────────────────
static void test_R_KEEP_priorbar_same_id_stop_survives_compaction() {
    std::printf("R-KEEP-prior-stop-later (physical same-id STOP survives compaction)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) strategy_close_all();
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),   // bar0: queue physical S
        mk(100, 102,  99, 100, 1'200'000),   // bar1: S fills; arm pending S@90
        mk(100, 102,  99, 100, 1'800'000),   // bar2: deferred close_all call
        mk(100, 102,  99, 100, 2'400'000),   // bar3: close fills; S remains untouched
        mk( 95,  95,  88,  90, 3'000'000),   // bar4: S@90 must still be live
        mk( 90,  91,  89,  90, 3'600'000),   // bar5: settle
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), -1.0));          // pre-fix: 0.0 (compaction removes S)
}

// ─────────────────────────────────────────────────────────────────────
// G-no-physical-lot. The logical id ledger and physical FIFO lot roster are
// intentionally made to disagree: close("B") consumes B's logical quantity
// but FIFO closes physical A, leaving only physical B. A later pending entry A
// therefore must NOT receive the same-id carve-out. This kills an implementation
// that consults id_unclosed_qty_ instead of pyramid_entries_.
// ─────────────────────────────────────────────────────────────────────
static void test_G_no_physical_same_id_stop_still_cancelled() {
    std::printf("G-no-physical-lot (logical A without physical A is cancelled)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("A", true);
            if (bar_index_ == 1) strategy_entry("B", true);
            if (bar_index_ == 2) strategy_close("B");
            if (bar_index_ == 3)
                strategy_entry("A", true, kNaN, /*stop=*/120.0, kNaN,
                               "logical-only same-id stop");
            if (bar_index_ == 4) strategy_close_all();
        }
    };
    Probe p;
    Bar bars[8] = {
        mk(100, 100, 100, 100,   600'000),   // bar0: queue A
        mk(100, 100, 100, 100, 1'200'000),   // bar1: A fills; queue B
        mk(100, 100, 100, 100, 1'800'000),   // bar2: B fills; close("B")
        mk(100, 100, 100, 100, 2'400'000),   // bar3: FIFO closes physical A; arm A@120
        mk(100, 100, 100, 100, 3'000'000),   // bar4: close_all call; only physical B exists
        mk(100, 100, 100, 100, 3'600'000),   // bar5: close_all fills; A@120 cancelled
        mk(125, 130, 125, 128, 4'200'000),   // bar6: would trigger A if wrongly preserved
        mk(128, 128, 128, 128, 4'800'000),   // bar7: settle
    };
    p.run(bars, 8);
    CHECK(p.trade_count() == 2);
    CHECK(near(p.pos_size(), 0.0));
}

// ─────────────────────────────────────────────────────────────────────
// G-close-id. Physical same-id provenance is not enough by itself: this new
// exception is pinned to deferred close_all only. A prior-bar same-id STOP
// remains cancelled after a full strategy.close(id).
// ─────────────────────────────────────────────────────────────────────
static void test_G_close_id_same_id_priorbar_stop_still_cancelled() {
    std::printf("G-close-id (same-id STOP remains cancelled by close(id))\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) strategy_close("S");
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),
        mk(100, 102,  99, 100, 1'800'000),
        mk(100, 102,  99, 100, 2'400'000),   // close("S") fills; pending S cancelled
        mk( 95,  95,  88,  90, 3'000'000),   // would trigger S if scope leaked
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));
}

// ─────────────────────────────────────────────────────────────────────
// G-LIMIT-characterization. Prior-bar pure LIMIT carry already has its own
// proven carve-out and must remain unchanged by the new pure-STOP provenance.
// ─────────────────────────────────────────────────────────────────────
static void test_G_same_id_priorbar_limit_carry_unchanged() {
    std::printf("G-LIMIT (existing prior-bar pure LIMIT carry unchanged)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1)
                strategy_entry("L", true, /*limit=*/90.0, kNaN, kNaN,
                               "prior-bar same-id limit");
            if (bar_index_ == 2) strategy_close_all();
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),
        mk(100, 102,  99, 100, 1'800'000),
        mk(100, 102,  99, 100, 2'400'000),   // close_all fills; L@90 remains resting
        mk( 95,  96,  88,  90, 3'000'000),   // L@90 fills
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 1.0));
}

// ─────────────────────────────────────────────────────────────────────
// G-stop-limit. Reusing a physically-live id is still insufficient when the
// pending entry has both stop and limit legs. The new exception is pure STOP
// only; this prior-bar same-id stop-limit remains tied to the closed cycle and
// must be cancelled.
// ─────────────────────────────────────────────────────────────────────
static void test_G_same_id_priorbar_stop_limit_still_cancelled() {
    std::printf("G-stop-limit (same-id prior-bar stop-limit remains cancelled)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1)
                strategy_entry("L", true, /*limit=*/115.0, /*stop=*/110.0,
                               kNaN, "prior-bar same-id stop-limit");
            if (bar_index_ == 2) strategy_close_all();
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),   // physical L; arm stop-limit
        mk(100, 102,  99, 100, 1'800'000),   // close_all call
        mk(100, 102,  99, 100, 2'400'000),   // close_all fills; stop-limit cancelled
        mk(100, 120,  99, 115, 3'000'000),   // would trigger and fill if preserved
        mk(115, 116, 114, 115, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));
}

// ─────────────────────────────────────────────────────────────────────
// G-RAW-before-close_all. The pending STOP has valid physical same-id
// provenance when close_all is CALLED, but an earlier-created opposite RAW
// market order is the instruction that actually flattens at the next open.
// Both orders share one created_bar, so call-bar provenance alone incorrectly
// attributes the RAW flatten to close_all and preserves S. Actual close-order
// identity must keep S in the stale-cycle wipe.
// ─────────────────────────────────────────────────────────────────────
static void test_G_raw_before_close_all_does_not_authorize_same_id_stop() {
    std::printf("G-RAW-before-close_all (RAW flatten cannot authorize STOP)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) {
                strategy_order("RAW", true, /*qty=*/1.0);
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),   // physical S; arm S@90
        mk(100, 102,  99, 100, 1'800'000),   // RAW first, then close_all
        mk(100, 102,  99, 100, 2'400'000),   // RAW actually flattens; S must drop
        mk( 95,  95,  88,  90, 3'000'000),   // would trigger leaked S
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));           // call-bar-only patch: -1.0
}

// ─────────────────────────────────────────────────────────────────────
// G-ANY-close-before-close_all. Same collision through a high-level EXIT:
// under close_entries_rule="ANY", close("S") keeps from_entry=S and therefore
// coexists with the later global close_all. It is earlier in source/created_seq
// and actually flattens. Sharing close_all's call bar must not grant S the
// close_all-only STOP preservation.
// ─────────────────────────────────────────────────────────────────────
static void test_G_any_close_id_before_close_all_does_not_authorize_same_id_stop() {
    std::printf("G-ANY-close-before-close_all (close(id) flatten cannot authorize STOP)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) { close_entries_rule_any_ = true; }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) {
                strategy_close("S");
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),   // physical S; arm S@90
        mk(100, 102,  99, 100, 1'800'000),   // close(S) first, then close_all
        mk(100, 102,  99, 100, 2'400'000),   // close(S) actually flattens
        mk( 95,  95,  88,  90, 3'000'000),   // would trigger leaked S
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));           // call-bar-only patch: -1.0
}

// ─────────────────────────────────────────────────────────────────────
// R-KEEP-close_all-replacement. A second close_all on the same call bar
// replaces the first deferred global close. The STOP provenance must refresh
// to the replacement order's fresh incarnation; binding it to the cancelled
// first order would make the real second close fail the identity gate and lose S.
// ─────────────────────────────────────────────────────────────────────
static void test_R_KEEP_replaced_close_all_refreshes_stop_identity() {
    std::printf("R-KEEP-close_all-replacement (STOP follows surviving close identity)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) {
                strategy_close_all();
                strategy_close_all();
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),
        mk(100, 102,  99, 100, 1'800'000),   // first close replaced by second
        mk(100, 102,  99, 100, 2'400'000),   // surviving close_all fills
        mk( 95,  95,  88,  90, 3'000'000),   // S remains live and fires
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), -1.0));
}

// ─────────────────────────────────────────────────────────────────────
// G-incarnation-replacement. created_seq is deliberately replacement-stable:
// a same-id order preserves its sorting slot. Therefore a RAW order using the
// synthetic close_all id "__close__" inherits close_all's created_seq while
// replacing that EXIT. The RAW fill must not impersonate the cancelled
// close_all; preservation needs a fresh, never-reused order incarnation.
// ─────────────────────────────────────────────────────────────────────
static void test_G_raw_same_id_replacement_cannot_impersonate_close_all() {
    std::printf("G-incarnation-replacement (RAW __close__ cannot impersonate close_all)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) {
                strategy_close_all();
                // Replaces internal pending id "__close__" and intentionally
                // inherits its created_seq ordering slot.
                strategy_order("__close__", true, /*qty=*/1.0);
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),
        mk(100, 102,  99, 100, 1'800'000),   // close_all replaced by RAW
        mk(100, 102,  99, 100, 2'400'000),   // RAW flattens; S must drop
        mk( 95,  95,  88,  90, 3'000'000),   // would trigger leaked S
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), 0.0));           // created_seq identity patch: -1.0
}

// ─────────────────────────────────────────────────────────────────────
// R-KEEP-close_all-before-ANY-close. The later close("S") is also a full
// close, but under close_entries_rule="ANY" it coexists with the earlier bare
// close_all rather than cancelling it. close_all fills first by source order,
// so its physically-live same-ID STOP provenance must remain intact. A global
// "clear every stamp on any later full close" loses S incorrectly.
// ─────────────────────────────────────────────────────────────────────
static void test_R_KEEP_close_all_before_any_close_id_preserves_stop() {
    std::printf("R-KEEP-close_all-before-ANY-close (surviving close_all owns stamp)\n");
    class Probe : public ProbeBase {
    public:
        Probe() : ProbeBase(4) { close_entries_rule_any_ = true; }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("S", false);
            if (bar_index_ == 1)
                strategy_entry("S", false, kNaN, /*stop=*/90.0, kNaN,
                               "prior-bar same-id stop");
            if (bar_index_ == 2) {
                strategy_close_all();
                strategy_close("S");
            }
        }
    };
    Probe p;
    Bar bars[6] = {
        mk(100, 100, 100, 100,   600'000),
        mk(100, 102,  99, 100, 1'200'000),
        mk(100, 102,  99, 100, 1'800'000),   // close_all first; close(S) coexists
        mk(100, 102,  99, 100, 2'400'000),   // close_all actually flattens
        mk( 95,  95,  88,  90, 3'000'000),   // S must remain live and fire
        mk( 90,  91,  89,  90, 3'600'000),
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    CHECK(near(p.pos_size(), -1.0));          // global stamp clearing: 0.0
}

// The ordinary COOF and magnifier schedulers must report the incarnation of
// the order that actually flattened the position. A real close_all authorizes
// its bound prior-bar STOP; a RAW same-id replacement must not impersonate it.
static void test_R_KEEP_coof_and_magnifier_close_all_incarnation() {
    std::printf("R-KEEP-COOF (close_all incarnation survives both schedulers)\n");
    run_coof_incarnation_control(/*replace_close_all=*/false,
                                 /*magnifier=*/false);
    run_coof_incarnation_control(/*replace_close_all=*/false,
                                 /*magnifier=*/true);
}

static void test_G_coof_and_magnifier_raw_replacement_incarnation() {
    std::printf("G-COOF (RAW replacement cannot impersonate in either scheduler)\n");
    run_coof_incarnation_control(/*replace_close_all=*/true,
                                 /*magnifier=*/false);
    run_coof_incarnation_control(/*replace_close_all=*/true,
                                 /*magnifier=*/true);
}

int main() {
    test_R_KEEP_mkt_undercap_survives();
    test_R_KEEP_stop_undercap_survives();
    test_G_DROP_mkt_overcap_probe65();
    test_G_DROP_stop_overcap_bracket();
    test_G_DROP_mkt_overcap_pyr2();
    test_G_carry_priorbar_still_cancelled();
    test_G_opposite_unchanged_ki64();
    test_R_KEEP_priorbar_same_id_stop_touched_on_close_fill_bar();
    test_R_KEEP_priorbar_same_id_stop_survives_compaction();
    test_G_no_physical_same_id_stop_still_cancelled();
    test_G_close_id_same_id_priorbar_stop_still_cancelled();
    test_G_same_id_priorbar_limit_carry_unchanged();
    test_G_same_id_priorbar_stop_limit_still_cancelled();
    test_G_raw_before_close_all_does_not_authorize_same_id_stop();
    test_G_any_close_id_before_close_all_does_not_authorize_same_id_stop();
    test_R_KEEP_replaced_close_all_refreshes_stop_identity();
    test_G_raw_same_id_replacement_cannot_impersonate_close_all();
    test_R_KEEP_close_all_before_any_close_id_preserves_stop();
    test_R_KEEP_coof_and_magnifier_close_all_incarnation();
    test_G_coof_and_magnifier_raw_replacement_incarnation();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
