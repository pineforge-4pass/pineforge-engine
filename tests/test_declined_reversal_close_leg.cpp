/*
 * test_declined_reversal_close_leg.cpp — declined-reversal close-leg
 * suppression (design-declined-reversal-close-leg.md).
 *
 * Cluster shape (traced, POOC=false): on a mutually-exclusive opposite signal
 * while in a percent-of-equity all-in (pct=100) position, the strategy queues
 *   strategy.entry(opposite)   [created FIRST]
 *   strategy.close(current)    [created AFTER, same on_bar]
 * pyramiding=0/1. Next bar the KI-57/KI-54 reversal-admission gate DECLINES the
 * opposite entry at fill (fill_open > sizing_close by >= one mintick, qty_step
 * 0), because an all-in flip's frozen notional sits within lot-floor slack of
 * equity. TradingView refuses the whole reversal ATOMICALLY and HOLDS the
 * position; the pre-fix engine let the co-queued strategy.close FILL anyway and
 * went FLAT, then re-entered on a later mid-span signal TV no-ops (+1 cAbs
 * each). The fix: when the reversal decline fires, the same-bar, later-created,
 * held-side FULL strategy.close leg it was paired with is suppressed too.
 *
 * Harness: modelled on test_margin_admission_gate.cpp (Probe subclass; scripted
 * per-bar actions). initial_capital 10000, PERCENT_OF_EQUITY pct=100, zero
 * commission, margin_call disabled, qty_step_=0 (so a +1-mintick fill-gap
 * DECLINES the reversal — at qty_step_>0 one tick ADMITS per KI-54 pin H). All
 * decline fixtures gap the fill bar +1 (>= one mintick) above the signal close.
 *
 * RED/GREEN matrix (design doc):
 *   R1 declined reversal suppresses same-bar later-created full close (RED).
 *   R2 admitted reversal unchanged (entry fills; close no-op).
 *   R3 close WITHOUT a paired reversal fires.
 *   R4 follow-up close on a DIFFERENT bar fires (one-shot binding) + ledger
 *      re-credit proof (post-fix the deferred close's consumed id-ledger is
 *      restored so a later close(id) can fire).
 *   R5 close_all + declined reversal: characterization freeze (NOT suppressed).
 *   R6 same-direction decline (probe65 shape) + co-queued close: close FIRES.
 *   R7 close created BEFORE the entry (chawarat sell leg): fires.
 *   G  two declined reversals same bar -> idempotent suppression.
 *   G  partial close excluded (not suppressed).
 *   G  multiple pending orders: only the matching close is suppressed.
 *   G  strategy.exit bracket NOT suppressed (+ call-time cancel caveat).
 *   G  pin-D fixture (declined reversal, NO co-queued close) unchanged.
 *   G  COOF-kernel cross-check (calc_on_order_fills=1) — RED in the KI-60
 *      kernel, same bug/other fill loop.
 *   G  POOC inertness (process_orders_on_close=1: no deferred close exists).
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

static Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Per-bar scripted actions. Order within a bar is preserved (creation order),
// which is load-bearing for R1 (entry-before-close) vs R7 (close-before-entry).
enum class Op { EnterLong, EnterShort, EnterLongAdd, CloseId, CloseL, CloseAll };
struct Action { Op op; };

class Probe : public BacktestEngine {
public:
    Probe(int pyramiding = 1) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        pyramiding_ = pyramiding;
        margin_call_enabled_ = false;
        syminfo_mintick_ = 0.01;
    }
    std::vector<std::vector<Action>> plan;   // plan[bar_index] = actions
    void on_bar(const Bar&) override {
        if (bar_index_ < 0 || bar_index_ >= (int)plan.size()) return;
        for (const auto& a : plan[bar_index_]) {
            switch (a.op) {
                case Op::EnterLong:     strategy_entry("L", true); break;
                case Op::EnterShort:    strategy_entry("S", false); break;
                case Op::EnterLongAdd:  strategy_entry("L2", true); break;
                case Op::CloseId:       strategy_close("L"); break;
                case Op::CloseL:        strategy_close("L"); break;
                case Op::CloseAll:      strategy_close(""); break;
            }
        }
    }
    double pos() const { return signed_position_size(); }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    using BacktestEngine::calc_on_order_fills_;
    using BacktestEngine::process_orders_on_close_;
    using BacktestEngine::id_unclosed_qty_;
};

// Canonical LONG-then-reversal bars. The LONG opens at 100 all-in (qty 100),
// the signal bar closes at 110 (open profit 1000 -> eq_S 11000, frozen short
// qty 100, sizing 110), and the fill bar OPENS at `fill_open`. fill_open=111
// (+1) DECLINES the short reversal; fill_open=110 (tie) ADMITS it.
static std::vector<Bar> reversal_bars(double fill_open) {
    return {
        mk(1000, 100, 100, 100, 100),                       // bar0: place L
        mk(2000, 100, 112,  99, 110),                       // bar1: L fills @100
        mk(3000, fill_open, fill_open + 1, fill_open - 1, fill_open),  // bar2
        mk(4000, fill_open, fill_open, fill_open, fill_open),          // bar3
        mk(5000, fill_open, fill_open, fill_open, fill_open),          // bar4
        mk(6000, fill_open, fill_open, fill_open, fill_open),          // bar5
    };
}

}  // namespace

// R1: declined reversal suppresses the same-bar, later-created FULL close.
// bar1 queues S (reversal, first) then close("L") (after). bar2 opens +1 -> S
// declines. POST-FIX: LONG held, no trade row. PRE-FIX (RED): close fires,
// engine goes FLAT with one close trade.
static void test_R1_declined_reversal_suppresses_close() {
    std::printf("-- R1: declined reversal suppresses co-queued full close --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},                       // bar0
        {{Op::EnterShort}, {Op::CloseL}},        // bar1: S first, close after
        {}, {}, {}, {},
    };
    auto bars = reversal_bars(111);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);   // RED pre-fix: FLAT
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 0);                     // RED pre-fix: 1 (close fired)
}

// R2: admitted reversal is unchanged — the entry flips, the close is a no-op.
// Same script, fill bar at the exact tie (fill_open=110) so S ADMITS.
static void test_R2_admitted_reversal_close_noop() {
    std::printf("-- R2: admitted reversal flips; close is a no-op --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::EnterShort}, {Op::CloseL}},
        {}, {}, {}, {},
    };
    auto bars = reversal_bars(110);                  // tie -> admit
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::SHORT);  // flip happened
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 1);                     // the L round-trip
}

// R3: a close with NO paired reversal fires (no over-suppression).
static void test_R3_close_without_reversal_fires() {
    std::printf("-- R3: close without paired reversal fires --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},                           // bar0: place L
        {{Op::CloseL}},                              // bar1: L fills; close("L")
        {}, {},
    };
    std::vector<Bar> bars = {
        mk(1000, 100, 100, 100, 100),
        mk(2000, 100, 100, 100, 100),                // L fills @100; close queued
        mk(3000, 100, 100, 100, 100),                // close fires -> FLAT
        mk(4000, 100, 100, 100, 100),
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::FLAT);
    CHECK(p.trade_count() == 1);
}

// R4: one-shot binding + ledger re-credit. bar1 suppresses the close (R1);
// bar3 issues a NEW close("L") on a DIFFERENT bar with no reversal — it must
// fire. Post-fix this only works if the deferred close's consumed id-ledger
// was re-credited on suppression; without the re-credit compute_close_target_qty
// finds unclosed=0 and the follow-up close no-ops (position holds forever).
// Pre-fix the bar1 close already fired, so the end state (FLAT, one trade) is
// the same characterization — it PASSES pre-fix and catches a missing re-credit
// post-fix.
static void test_R4_followup_close_and_ledger_recredit() {
    std::printf("-- R4: follow-up close on a later bar fires (ledger re-credit) --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},                           // bar0
        {{Op::EnterShort}, {Op::CloseL}},            // bar1: reversal + close
        {},                                          // bar2: S declines
        {{Op::CloseL}},                              // bar3: fresh close("L")
        {},                                          // bar4: fires -> FLAT
        {},
    };
    auto bars = reversal_bars(111);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::FLAT);   // follow-up close fired
    CHECK(p.trade_count() == 1);                     // exactly one L round-trip
    if (p.trade_count() == 1) {
        CHECK_NEAR(p.get_trade(0).exit_price, 111.0, 1e-9);   // closed at bar4 open
    }
}

// R5: close_all co-queued with a declined reversal is a characterization FREEZE
// — its bare "__close__" id (empty target) is EXCLUDED from suppression, so it
// still fires. LONG held then S + close_all(); S declines but close_all flattens.
static void test_R5_close_all_freeze() {
    std::printf("-- R5: close_all + declined reversal freeze (NOT suppressed) --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::EnterShort}, {Op::CloseAll}},
        {}, {}, {}, {},
    };
    auto bars = reversal_bars(111);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::FLAT);   // close_all still fired
    CHECK(p.trade_count() == 1);
}

// R6: the SAME-direction shape (probe65: same-id add + close, no reversal) is
// UNTOUCHED — the hook keys on the reversal decline only. Sequencing note: the
// sort processes a full close BEFORE a same-direction entry (sort_orders_by_
// fill_phase's exit-before-same-dir-entry rule), so a same-direction add
// co-queued with a full close can never reach the KI-54 same_dir DECLINE while
// a held-side close is pending — the close fires first and the add re-opens
// from flat. That makes a same_dir decline + held-side close structurally
// unreachable, so the `reversal==true` guard is only ever exercised on genuine
// reversals; this row pins the fix's inertness on the same-direction shape
// (close fires, add re-opens LONG 100 — byte-identical to HEAD).
static void test_R6_same_dir_shape_fix_inert() {
    std::printf("-- R6: same-direction add + close: fix inert (close fires) --\n");
    Probe p(/*pyramiding=*/2);
    p.plan = {
        {{Op::EnterLong}},                           // bar0: place L (frozen 100)
        {{Op::EnterLongAdd}, {Op::CloseL}},          // bar1: L fills; add L2 + close
        {},                                          // bar2: close fires; L2 re-opens
        {},
    };
    std::vector<Bar> bars = {
        mk(1000, 100, 100, 100, 100),
        mk(2000, 100, 100, 100, 100),                // L fills LONG 100 @100
        mk(3000, 100, 100, 100, 100),
        mk(4000, 100, 100, 100, 100),
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);   // close fired, add re-opened
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 1);                     // the original L round-trip
}

// R7: a close created BEFORE the reversal entry (chawarat sell-leg shape) is NOT
// suppressed — creation-order binding. bar1 queues close("L") FIRST then S; the
// close (lower created_seq) processes first and fires; S then opens from flat.
static void test_R7_close_created_before_entry_fires() {
    std::printf("-- R7: close created before entry fires --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::CloseL}, {Op::EnterShort}},            // close FIRST, then S
        {}, {}, {}, {},
    };
    auto bars = reversal_bars(111);
    p.run(bars.data(), (int)bars.size());
    // The long was closed at bar2 open (111), NOT held: one round-trip booked.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(p.get_trade(0).is_long);
        CHECK_NEAR(p.get_trade(0).exit_price, 111.0, 1e-9);
    }
}

// G: two declined reversals on the same bar -> idempotent suppression (the flag
// is set once, re-credit fires once). Two shorts S both decline; the single
// close("L") is suppressed exactly once and a later close still flattens 100.
static void test_G_two_reversals_idempotent() {
    std::printf("-- G: two declined reversals same bar, idempotent suppression --\n");
    Probe p(/*pyramiding=*/2);
    // Two short reversals in one bar: both target the LONG, both decline.
    class TwoShortProbe : public Probe {
    public:
        TwoShortProbe() : Probe(2) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1) {
                strategy_entry("S", false);          // reversal #1 (created 1st)
                strategy_entry("S2", false);         // reversal #2 (created 2nd)
                strategy_close("L");                 // close (created last)
            }
            if (bar_index_ == 3) strategy_close("L"); // follow-up
        }
    };
    TwoShortProbe tp;
    auto bars = reversal_bars(111);
    tp.run(bars.data(), (int)bars.size());
    CHECK(tp.trade_count() == 1);                    // one clean round-trip
    if (tp.trade_count() == 1) {
        CHECK_NEAR(tp.get_trade(0).qty, 100.0, 1e-9);  // exactly 100 closed once
    }
    CHECK(tp.position_side_ == PositionSide::FLAT);  // follow-up close flattened
}

// G: a PARTIAL close (qty_percent < 100) is excluded from suppression — no
// exemplar covers it, so current behavior freezes (the partial close fires).
static void test_G_partial_close_not_suppressed() {
    std::printf("-- G: partial close excluded (not suppressed) --\n");
    class PartialProbe : public Probe {
    public:
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1) {
                strategy_entry("S", false);
                strategy_close("L", "", kNaN, 50.0);   // 50% partial close
            }
        }
    };
    PartialProbe pp;
    auto bars = reversal_bars(111);
    pp.run(bars.data(), (int)bars.size());
    // S declines; the partial close is NOT suppressed and trims half the LONG.
    CHECK(pp.position_side_ == PositionSide::LONG);
    CHECK_NEAR(pp.position_qty_, 50.0, 1e-9);        // half closed
    CHECK(pp.trade_count() == 1);
}

// G: with extra unrelated pending orders in the book, only the matching full
// close is suppressed. Here a resting deep limit LONG entry (created a prior
// bar, never touched) coexists with the declined reversal + full close.
static void test_G_multiple_pending_orders() {
    std::printf("-- G: only the matching close is suppressed --\n");
    class MultiProbe : public Probe {
    public:
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1) {
                // A resting deep-limit LONG that never fills on these bars.
                strategy_entry("DEEP", true, /*limit=*/1.0);
                strategy_entry("S", false);
                strategy_close("L");
            }
        }
    };
    MultiProbe mp;
    auto bars = reversal_bars(111);
    mp.run(bars.data(), (int)bars.size());
    CHECK(mp.position_side_ == PositionSide::LONG);  // close suppressed, held
    CHECK_NEAR(mp.position_qty_, 100.0, 1e-9);
    CHECK(mp.trade_count() == 0);
}

// G: a strategy.exit bracket bound to "L" is NOT suppressed by the fix (it does
// not carry the "__close__" id). Caveat (design item 9): the FULL close's
// call-time cancel_orders_for_full_close already wiped the bracket at CALL time
// (bar1), so no bracket exit fires later even though the close is suppressed.
// Observable: LONG held (close suppressed), no bracket exit ever fires.
static void test_G_exit_bracket_not_suppressed() {
    std::printf("-- G: strategy.exit bracket not suppressed (call-time cancel caveat) --\n");
    class BracketProbe : public Probe {
    public:
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1) {
                strategy_exit("X", "L", 1000.0, 1.0, kNaN, kNaN, kNaN, 100.0, "");
                strategy_entry("S", false);
                strategy_close("L");
            }
        }
    };
    BracketProbe bp;
    auto bars = reversal_bars(111);
    bp.run(bars.data(), (int)bars.size());
    CHECK(bp.position_side_ == PositionSide::LONG);  // close suppressed
    CHECK_NEAR(bp.position_qty_, 100.0, 1e-9);
    CHECK(bp.trade_count() == 0);                    // bracket wiped at call time
}

// G: pin-D fixture (a declined reversal with NO co-queued close) is unchanged —
// the fix is inert without a close leg. This is the KI-54 pin D shape.
static void test_G_pin_D_unchanged() {
    std::printf("-- G: pin-D (declined reversal, no close) unchanged --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::EnterShort}},                          // reversal only, no close
        {}, {}, {}, {},
    };
    auto bars = reversal_bars(111);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);   // reversal declined, held
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 0);
}

// G: COOF-kernel cross-check. calc_on_order_fills=1 drives the KI-60
// process_next_pending_order loop, whose candidates are pre-classified BEFORE
// any candidate is applied — so a flag set mid-segment by the reversal's
// decline is NOT seen by classify and must be caught by the shared apply-time
// guard (verified: the guard fires here, classify fires in the ordinary
// kernel). To reproduce a genuine decline under COOF the reversal must NOT fill
// intrabar on its signal bar (where COOF would fill it at the same cursor its
// sizing was computed against — an admit): the LONG opens on bar0/1 and the
// S+close are queued on bar2 (a no-concurrent-fill signal bar with open
// profit), so they defer to bar3's +1 gap open and S declines there. RED in
// this kernel pre-fix (close fires -> FLAT); held post-fix.
static void test_G_coof_kernel_cross_check() {
    std::printf("-- G: COOF-kernel cross-check (calc_on_order_fills=1) --\n");
    Probe p;
    p.calc_on_order_fills_ = true;
    p.plan = {
        {{Op::EnterLong}},                           // bar0: place L
        {},                                          // bar1: L fills @100 (LONG)
        {{Op::EnterShort}, {Op::CloseL}},            // bar2: signal @110, S + close
        {}, {}, {},
    };
    std::vector<Bar> bars = {
        mk(1000, 100, 100, 100, 100),
        mk(2000, 100, 100, 100, 100),                // L fills @100
        mk(3000, 100, 112,  99, 110),                // profit; S + close queued
        mk(4000, 111, 112, 110, 111),                // bar3: +1 gap -> S declines
        mk(5000, 111, 111, 111, 111),
        mk(6000, 111, 111, 111, 111),
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);   // RED pre-fix: FLAT
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 0);
}

// G: POOC inertness. Under process_orders_on_close the strategy.close executes
// IMMEDIATELY at the signal-bar close, so no deferred close order is pending at
// the reversal-decline bar — the fix is structurally inert. This asserts the
// current (pre-fix == post-fix) behavior is preserved.
static void test_G_pooc_inertness() {
    std::printf("-- G: POOC inertness (no deferred close to suppress) --\n");
    Probe p;
    p.process_orders_on_close_ = true;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::EnterShort}, {Op::CloseL}},
        {}, {}, {}, {},
    };
    auto bars = reversal_bars(111);
    p.run(bars.data(), (int)bars.size());
    // Characterization (pinned from HEAD): under POOC the reversal S fills at
    // the SIGNAL bar's close (110 == sizing price, an exact tie) so it ADMITS
    // and flips to SHORT — there is no next-bar adverse gap and no deferred
    // close order at all, so the fix is structurally inert. Post-fix must match.
    CHECK(p.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
}

int main() {
    std::printf("--- declined_reversal_close_leg ---\n");
    test_R1_declined_reversal_suppresses_close();
    test_R2_admitted_reversal_close_noop();
    test_R3_close_without_reversal_fires();
    test_R4_followup_close_and_ledger_recredit();
    test_R5_close_all_freeze();
    test_R6_same_dir_shape_fix_inert();
    test_R7_close_created_before_entry_fires();
    test_G_two_reversals_idempotent();
    test_G_partial_close_not_suppressed();
    test_G_multiple_pending_orders();
    test_G_exit_bracket_not_suppressed();
    test_G_pin_D_unchanged();
    test_G_coof_kernel_cross_check();
    test_G_pooc_inertness();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
