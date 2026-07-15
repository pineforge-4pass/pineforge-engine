/*
 * test_pooc_position_visibility.cpp — KI-64 successor.
 *
 * THE RULE (process_orders_on_close=true only): during bar i's script
 * execution, ``strategy.position_size`` (signed_position_size()) must report
 * the position as it stood BEFORE any same-bar close fills. A
 * strategy.close/close_all ordered earlier in bar i fills at bar i's close per
 * POOC, but its effect on script-visible position state becomes visible only
 * from bar i+1. Broker/order state (position_side_, position_qty_, trades_)
 * mutates immediately as before — only the SCRIPT-facing accessor defers.
 *
 * Ground truth: data/probes/pf-probe-ki64-daypivot-crossover — TV places 0
 * entries on the 1,278 exit bars (a flat-gated strategy.entry on a close_all
 * bar is never placed); the pre-fix engine re-enters on 399/1,381 exit bars
 * because close_all flips position_size to 0 mid-on_bar.
 *
 * R rows are RED vs worktree HEAD 8b5932f (engine flips visibility). G rows
 * are characterization that must hold before AND after the fix:
 *   - POOC=false is unchanged (the close is a deferred market exit that fills
 *     next bar, so the position is never mutated mid-on_bar — no freeze needed).
 *   - an entry gated on position_size != 0 placed BEFORE the close still fires.
 *   - strategy.close(immediately=true) is DEFINED to reflect its fill at once,
 *     so it is NOT deferred (the :3896 same-dir immediate-cancel pin holds).
 *   - a NON-flat-gated opposite entry (reversal) still flips (affordable
 *     reversal class: sharpstrat/raphaeltay).
 *   - the freeze is scoped to the close bar: next-bar and post-run reads see
 *     the real (post-close) position.
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(double c, int64_t ts) {
    Bar b;
    b.open = c; b.high = c; b.low = c; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// Flat OHLCV series (price 100 throughout) — isolates order/position mechanics
// from fill-price effects. Under POOC a market order placed in bar i's on_bar
// fills at bar i's close.
static Bar bars4[4] = {
    mk(100,  600'000), mk(100, 1'200'000), mk(100, 1'800'000), mk(100, 2'400'000),
};

// ─────────────────────────────────────────────────────────────────────
// Shared probe kernel (mirrors the daypivot probe): enter while flat, then on
// the NEXT bar close_all and immediately re-test the flat gate. ``opener``
// selects how the close is issued so both the close_all and the
// strategy.close(id) paths through execute_immediate_close are exercised.
// ─────────────────────────────────────────────────────────────────────
enum class CloseKind { CloseAll, CloseIdAny };

class ProbeKernel : public BacktestEngine {
public:
    CloseKind kind;
    int  entry_bar = -1;
    int  entries_placed = 0;
    double gate_pos_on_close_bar = -999.0;  // position_size the flat gate saw on bar1
    double pos_on_next_bar = -999.0;        // position_size at start of bar2

    explicit ProbeKernel(CloseKind k, bool pooc) : kind(k) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = pooc;
        if (k == CloseKind::CloseIdAny) close_entries_rule_any_ = true;
    }
    void issue_close() {
        if (kind == CloseKind::CloseAll) strategy_close_all();
        else                             strategy_close("L");   // any-rule full close
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 2) pos_on_next_bar = signed_position_size();
        // close trigger: one bar after entry (probe's `bar_index > entry_bar`)
        if (signed_position_size() != 0.0 && entry_bar >= 0 && bar_index_ > entry_bar) {
            issue_close();
        }
        // flat-gated entry, armed on bars 0 and 1 (sig_up in the probe)
        if (bar_index_ == 1) gate_pos_on_close_bar = signed_position_size();
        if (signed_position_size() == 0.0 && (bar_index_ == 0 || bar_index_ == 1)) {
            strategy_entry("L", true);
            entry_bar = bar_index_;
            ++entries_placed;
        }
    }
    double ssize() const { return signed_position_size(); }
};

// R1 — POOC close_all: the flat gate on the close bar must see the PRE-close
// LONG 1 and NOT re-enter. RED pre-fix: gate sees 0, re-enters (2 entries).
static void test_R1_pooc_closeall_flat_gate_blocks_reentry() {
    std::printf("R1: POOC close_all — flat-gated entry blocked on close bar\n");
    ProbeKernel p(CloseKind::CloseAll, /*pooc=*/true);
    p.run(bars4, 4);
    CHECK(near(p.gate_pos_on_close_bar, 1.0));  // FROZEN pre-close (RED: 0.0)
    CHECK(p.entries_placed == 1);               // no bar1 re-entry (RED: 2)
}

// R2 — POOC strategy.close(id) that routes through execute_immediate_close
// (close_entries_rule=ANY full close). Same rule as R1.
static void test_R2_pooc_closeid_flat_gate_blocks_reentry() {
    std::printf("R2: POOC strategy.close(id) — flat-gated entry blocked\n");
    ProbeKernel p(CloseKind::CloseIdAny, /*pooc=*/true);
    p.run(bars4, 4);
    CHECK(near(p.gate_pos_on_close_bar, 1.0));  // FROZEN pre-close (RED: 0.0)
    CHECK(p.entries_placed == 1);               // no bar1 re-entry (RED: 2)
}

// G1 — POOC=false characterization: the close is a DEFERRED market exit that
// fills next bar's open, so position_size is never mutated mid-on_bar; the flat
// gate already sees the open position. Same numeric outcome as fixed POOC, via
// a different mechanism. Must be UNCHANGED by the fix (freeze never arms).
static void test_G1_non_pooc_unchanged() {
    std::printf("G1: POOC=false — deferred close, flat gate sees open (unchanged)\n");
    ProbeKernel p(CloseKind::CloseAll, /*pooc=*/false);
    p.run(bars4, 4);
    CHECK(near(p.gate_pos_on_close_bar, 1.0));   // real open position (never mutated)
    CHECK(p.entries_placed == 1);
}

// G2 — an entry gated on position_size != 0 placed BEFORE the close call in the
// same bar must still fire (the freeze arms only AT the close). pyramiding=2.
static void test_G2_pooc_entry_before_close_still_fires() {
    std::printf("G2: POOC — position_size!=0 entry BEFORE close still fires\n");
    class Strat : public BacktestEngine {
    public:
        bool add_placed = false;
        Strat() {
            initial_capital_ = 1'000'000; default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0; commission_value_ = 0.0; slippage_ = 0;
            pyramiding_ = 2; process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1 && signed_position_size() != 0.0) {
                strategy_entry("L_add", true);   // gated on != 0, BEFORE the close
                add_placed = true;
                strategy_close_all();
            }
        }
    };
    Strat s; s.run(bars4, 4);
    CHECK(s.add_placed);   // the != 0 gate saw the real LONG before the close
}

// R3/R4 — ordinary POOC close_all preserves a same-direction MARKET entry that
// was created BEFORE the close while still under the pyramiding cap. The close
// fills at C, then the surviving entry opens a fresh position at that same C.
// Anchored by a production long-side oracle and characterized symmetrically.
static void test_pooc_undercap_entry_before_closeall_survives(bool held_long) {
    std::printf("R3/R4: POOC under-cap %s entry before close_all survives\n",
                held_long ? "long" : "short");
    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool held_long) : held_long_(held_long) {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 2;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("BASE", held_long_);
            const double pos = signed_position_size();
            const bool holding = held_long_ ? pos > 0.0 : pos < 0.0;
            if (bar_index_ == 1 && holding) {
                strategy_entry("ADD", held_long_);  // UNDER cap, before close
                strategy_close_all();
            }
        }
        double ssize() const { return signed_position_size(); }
        std::string entry_id() const { return open_trade_entry_id(0); }
    private:
        bool held_long_;
    } s(held_long);

    s.run(bars4, 4);
    CHECK(s.trade_count() == 1);  // BASE closed by close_all
    CHECK(near(s.ssize(), held_long ? 1.0 : -1.0));  // ADD survived and reopened
    CHECK(s.entry_id() == "ADD");
}

// Control — the same source order at the pyramiding cap remains rejected. The
// close must flatten BASE without allowing the over-cap-at-placement ADD to reopen.
static void test_pooc_overcap_entry_before_closeall_drops(bool held_long) {
    std::printf("control: POOC over-cap %s entry before close_all drops\n",
                held_long ? "long" : "short");
    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool held_long) : held_long_(held_long) {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 1;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("BASE", held_long_);
            const double pos = signed_position_size();
            const bool holding = held_long_ ? pos > 0.0 : pos < 0.0;
            if (bar_index_ == 1 && holding) {
                strategy_entry("ADD", held_long_);  // OVER cap, before close
                strategy_close_all();
            }
        }
        double ssize() const { return signed_position_size(); }
    private:
        bool held_long_;
    } s(held_long);

    s.run(bars4, 4);
    CHECK(s.trade_count() == 1);
    CHECK(near(s.ssize(), 0.0));
}

// COOF control — the production oracle has calc_on_order_fills=false, so the
// ordinary-POOC carve-out above must not leak into the COOF scheduler. Keep the
// established COOF full-close cleanup: an under-cap same-direction MARKET add
// created before close_all at the ordinary C execution is cancelled. Direction
// symmetry guards both LONG and SHORT cleanup predicates.
static void test_coof_pooc_undercap_entry_before_closeall_still_cancels(
        bool held_long) {
    std::printf("control: COOF+POOC under-cap %s entry before close_all cancels\n",
                held_long ? "long" : "short");
    class Strat : public BacktestEngine {
    public:
        explicit Strat(bool held_long) : held_long_(held_long) {
            initial_capital_ = 1'000'000;
            default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0;
            commission_value_ = 0.0;
            slippage_ = 0;
            pyramiding_ = 2;
            process_orders_on_close_ = true;
            calc_on_order_fills_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0 && !base_placed_) {
                base_placed_ = true;
                strategy_entry("BASE", held_long_);
            }
            const double pos = signed_position_size();
            const bool holding = held_long_ ? pos > 0.0 : pos < 0.0;
            if (bar_index_ == 1 && holding && !close_cluster_placed_) {
                close_cluster_placed_ = true;
                strategy_entry("ADD", held_long_);  // UNDER cap, before close
                strategy_close_all();
            }
        }
        double ssize() const { return signed_position_size(); }
        bool close_cluster_placed() const { return close_cluster_placed_; }
    private:
        bool held_long_;
        bool base_placed_ = false;
        bool close_cluster_placed_ = false;
    } s(held_long);

    s.run(bars4, 4);
    CHECK(s.close_cluster_placed());
    CHECK(s.trade_count() == 1);
    CHECK(near(s.ssize(), 0.0));
}

// G3 — strategy.close(immediately=true) is DEFINED to reflect its fill at once,
// so it must NOT be deferred: the mid-bar read is 0 and the prior same-dir
// market re-entry is cancelled (test_integration :3896 shape). pyramiding=2.
static void test_G3_pooc_immediately_not_deferred() {
    std::printf("G3: POOC immediately=true — NOT deferred (visible at once)\n");
    class Strat : public BacktestEngine {
    public:
        double mid_bar_pos = -999.0;
        Strat() {
            initial_capital_ = 1'000'000; default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0; commission_value_ = 0.0; slippage_ = 0;
            pyramiding_ = 2; process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_entry("L_add", true);
                strategy_close("L", "", kNaN, kNaN, /*immediately=*/true);
                mid_bar_pos = signed_position_size();   // immediate: reads 0
            }
        }
        double ssize() const { return signed_position_size(); }
    };
    Strat s; s.run(bars4, 4);
    CHECK(near(s.mid_bar_pos, 0.0));    // immediate=true is visible at once
    CHECK(s.trade_count() == 1);        // L_add same-dir re-entry cancelled
    CHECK(near(s.ssize(), 0.0));        // ends flat
}

// G5a — pure reversal: a NON-flat-gated opposite entry while LONG flips to
// SHORT under POOC (affordable-reversal class). No close, no freeze.
static void test_G5a_pooc_pure_reversal_flips() {
    std::printf("G5a: POOC — opposite entry (no close) flips LONG->SHORT\n");
    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 1'000'000; default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0; commission_value_ = 0.0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1 && signed_position_size() > 0.0)
                strategy_entry("S", false);   // reversal, NOT gated on == 0
        }
        double ssize() const { return signed_position_size(); }
    };
    Strat s; s.run(bars4, 4);
    CHECK(s.ssize() < 0.0);   // flipped to SHORT
}

// G5b — close_all THEN an unconditional opposite entry same bar: the freeze
// must NOT block the reversal (S is not flat-gated). Ends SHORT.
static void test_G5b_pooc_closeall_then_opposite_entry_flips() {
    std::printf("G5b: POOC — close_all + opposite entry still flips to SHORT\n");
    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 1'000'000; default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0; commission_value_ = 0.0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1 && signed_position_size() > 0.0) {
                strategy_close_all();
                strategy_entry("S", false);   // opposite, unconditional
            }
        }
        double ssize() const { return signed_position_size(); }
    };
    Strat s; s.run(bars4, 4);
    CHECK(s.ssize() < 0.0);   // reversal survived the freeze
}

// G6 — next-bar visibility: after a close flattens on bar1, bar2's on_bar reads
// the real FLAT position (freeze is scoped to the arming bar).
static void test_G6_pooc_next_bar_reads_flat() {
    std::printf("G6: POOC — next bar reads the real (flat) position\n");
    ProbeKernel p(CloseKind::CloseAll, /*pooc=*/true);
    p.run(bars4, 4);
    CHECK(near(p.pos_on_next_bar, 0.0));   // bar2 sees post-close FLAT
}

// G7 — post-run read after a close on the LAST bar must return the real
// (flat) position, not the frozen snapshot (guards the flush-time clear).
static void test_G7_pooc_post_run_read_is_real() {
    std::printf("G7: POOC — post-run read after last-bar close_all is FLAT\n");
    class Strat : public BacktestEngine {
    public:
        Strat() {
            initial_capital_ = 1'000'000; default_qty_type_ = QtyType::FIXED;
            default_qty_value_ = 1.0; commission_value_ = 0.0; slippage_ = 0;
            process_orders_on_close_ = true;
        }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) strategy_entry("L", true);
            if (bar_index_ == 1 && signed_position_size() > 0.0) strategy_close_all();
        }
        double ssize() const { return signed_position_size(); }
    };
    Strat s;
    Bar bars2[2] = { mk(100, 600'000), mk(100, 1'200'000) };  // close on last bar
    s.run(bars2, 2);
    CHECK(near(s.ssize(), 0.0));   // post-run: real flat (RED-if-broken: 1.0 frozen)
}

int main() {
    test_R1_pooc_closeall_flat_gate_blocks_reentry();
    test_R2_pooc_closeid_flat_gate_blocks_reentry();
    test_G1_non_pooc_unchanged();
    test_G2_pooc_entry_before_close_still_fires();
    test_pooc_undercap_entry_before_closeall_survives(/*held_long=*/true);
    test_pooc_undercap_entry_before_closeall_survives(/*held_long=*/false);
    test_pooc_overcap_entry_before_closeall_drops(/*held_long=*/true);
    test_pooc_overcap_entry_before_closeall_drops(/*held_long=*/false);
    test_coof_pooc_undercap_entry_before_closeall_still_cancels(
        /*held_long=*/true);
    test_coof_pooc_undercap_entry_before_closeall_still_cancels(
        /*held_long=*/false);
    test_G3_pooc_immediately_not_deferred();
    test_G5a_pooc_pure_reversal_flips();
    test_G5b_pooc_closeall_then_opposite_entry_flips();
    test_G6_pooc_next_bar_reads_flat();
    test_G7_pooc_post_run_read_is_real();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
