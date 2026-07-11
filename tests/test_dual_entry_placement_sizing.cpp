/*
 * test_dual_entry_placement_sizing.cpp — KI-65: dual same-bar opposite
 * strategy.entry from FLAT with placement-time sizing + pending-market
 * awareness.
 *
 * PIN (data/progress/probe-matrix-adjudication-20260711.md §KI-65, probe
 * data/probes/pf-probe-ki65-dual-entry-precedence, 7,429 TV trades):
 * from flat, pyramiding=0, POOC=false, explicit fixed qty — TV runs NO
 * arbitration on two opposite same-bar strategy.entry calls; BOTH execute.
 * Sizing freezes at PLACEMENT with pending-market awareness: the SECOND call
 * sells its own qty PLUS the qty of the pending same-bar OPPOSITE MARKET
 * entry (a pending STOP contributes 0; a placement-REJECTED entry contributes
 * 0). Net: the second (priced) leg FULLY REVERSES the position the first
 * (market) leg opens — it does NOT collapse to close-only-flat.
 *
 * MECHANISM (engine): a flat-armed priced (stop/limit) entry that reverses a
 * position opened THIS bar by an EARLIER opposite-direction MARKET entry took
 * the M2a close_only_opposite gate (apply_entry_order_fill →
 * close_opposite_then_enter): it closed the market leg and stayed FLAT,
 * dropping the second leg. TV holds the second leg. The fix scopes the gate
 * OUT of these market-first cells via a placement-time flag
 * (reverses_same_bar_market_from_flat) so the fill takes the ordinary
 * full-reversal path (flip_market_position_to, close_only=false).
 *
 * R1/R2/R3 are RED vs worktree HEAD (eaf5e97): today the second leg is
 * dropped (pos ends FLAT). G1–G3 are byte-stable characterization — the fix
 * MUST NOT change them (they pin the discriminator: STOP-first and
 * placement-rejected still close-only; MM-both-market unchanged path).
 *
 * Cell map (probe hours, TRUE combo under the probe's v6 float-div coverage):
 *   MS-LF-A (hh=04, 391 ev): E1 long MARKET, E2 short STOP → TV: long dur0,
 *                            short HELD.  Engine HEAD: close-only-flat.  [R1]
 *   MS-SF-A (hh=06/10, 782): E1 short MARKET, E2 long STOP → TV: net +1 long
 *                            HELD.  Engine HEAD: close-only-flat.        [R2]
 *   SS-LF-A (hh=08, 391 ev): E1 long STOP, E2 short STOP → single close,
 *                            net FLAT (BOTH match — must stay).          [G1]
 *   MM-*-A  (hh=00/02):      both MARKET, net already matches (diff path).[G2]
 *   -U cells / placement-reject: E1 over-notional → dropped, contributes 0. [G3]
 * Out of scope (frozen characterization): SS-SF (never ran in the probe),
 * pyramiding>0 multi-bar pyramids, deferred_flip carry (own suite).
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

// Flat bar at price `p` (o=h=l=c=p): a stop placed above/below the price is
// already marketable, so it fills at the shared open on the next bar exactly
// like the co-queued market leg (the dur-0 collision the pin describes).
static Bar mk(double p, int64_t ts) {
    Bar b;
    b.open = p; b.high = p; b.low = p; b.close = p;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// Common probe framing: from flat, pyramiding=0, 1x margin, no slip/comm.
struct DualProbeBase : public BacktestEngine {
    DualProbeBase(double capital = 1'000'000) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 0;
        margin_long_ = 100;
        margin_short_ = 100;
        syminfo_mintick_ = 0.01;
    }
    double pos() const { return signed_position_size(); }
};

// ─────────────────────────────────────────────────────────────────────
// R1 — MS-LF-A: E1 long MARKET, then E2 short STOP (marketable @200), same
// bar, from flat, both affordable. TV: E1 long fills dur-0 and is closed by
// E2's fill; E2 short is HELD (net -1). Engine HEAD: close-only-flat (net 0).
// EXACT TV trade shape is reproducible here (E1 fills before E2 by seq).
// ─────────────────────────────────────────────────────────────────────
static void test_R1_ms_lf_a_short_held() {
    std::printf("test_R1_ms_lf_a_short_held\n");
    struct P : DualProbeBase {
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E1", true,  kNaN, kNaN,   1.0, "MS-LF-1");  // long market
                strategy_entry("E2", false, kNaN, 200.0,  1.0, "MS-LF-2");  // short stop, marketable
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    // THE FIX: the short leg is held, not dropped.
    CHECK(near(p.pos(), -1.0));                 // HEAD: 0.0 (close-only-flat)
    // Exactly one closed trade: E1's long, opened and closed on the fill bar.
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_id == "E1");
        CHECK(near(t.qty, 1.0));
        CHECK(near(t.entry_price, 100.0));
        CHECK(near(t.exit_price, 100.0));
        CHECK(t.entry_bar_index == 1);
        CHECK(t.exit_bar_index == 1);           // dur-0 round trip
    }
}

// ─────────────────────────────────────────────────────────────────────
// R2 — MS-SF-A: E1 short MARKET, then E2 long STOP (marketable @50), same
// bar, from flat. TV holds net +1 long (the buy-side E2 leg). Engine HEAD:
// close-only-flat (net 0). The engine fills E1 (short) first by seq, so the
// trade decomposition differs from TV's buy-first split (E2 long x2); we pin
// the NET position, which is the 782-event divergence the pin names.
// ─────────────────────────────────────────────────────────────────────
static void test_R2_ms_sf_a_long_held() {
    std::printf("test_R2_ms_sf_a_long_held\n");
    struct P : DualProbeBase {
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E1", false, kNaN, kNaN,  1.0, "MS-SF-1");  // short market
                strategy_entry("E2", true,  kNaN, 50.0,  1.0, "MS-SF-2");  // long stop, marketable
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(near(p.pos(), 1.0));                   // HEAD: 0.0 (close-only-flat)
    CHECK(p.trade_count() == 1);                 // E1 short round-trip, dur-0
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(!t.is_long);
        CHECK(t.entry_id == "E1");
        CHECK(t.exit_bar_index == 1);
    }
}

// ─────────────────────────────────────────────────────────────────────
// R3 — placement-time sizing = own + pending-opposite-MARKET qty. E1 long
// MARKET qty 1; E2 short STOP own qty 2. The second call reverses: it sells
// |old|(1) to close the market leg AND opens its OWN qty(2) → total moved 3,
// net -2 short. (close-only HEAD → 0; a naive "open own+market=3" → -3; the
// pinned flip-opens-own-qty semantics → -2.) Proves the close consumes the
// market leg and the open leg is the second call's own qty.
// ─────────────────────────────────────────────────────────────────────
static void test_R3_second_call_sizing_two_lot() {
    std::printf("test_R3_second_call_sizing_two_lot\n");
    struct P : DualProbeBase {
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E1", true,  kNaN, kNaN,   1.0, "MS-LF-1");  // long market qty 1
                strategy_entry("E2", false, kNaN, 200.0,  2.0, "MS-LF-2");  // short stop own qty 2
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(near(p.pos(), -2.0));                  // HEAD: 0.0
    CHECK(p.trade_count() == 1);                 // only E1's long closed
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(near(t.qty, 1.0));                 // closed exactly the market leg's qty
    }
}

// ─────────────────────────────────────────────────────────────────────
// G1 — SS-LF-A: BOTH legs are STOPS (E1 long stop @50, E2 short stop @200),
// marketable, from flat. E2's pending opposite is a STOP → contributes 0 →
// E2 sells own(1) = closes E1's long exactly → net FLAT. Byte-stable: HEAD
// and post-fix both flat (the discriminator — a STOP-first cell must NOT
// gain a held reverse leg).
// ─────────────────────────────────────────────────────────────────────
static void test_G1_ss_lf_a_single_close_flat() {
    std::printf("test_G1_ss_lf_a_single_close_flat\n");
    struct P : DualProbeBase {
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E1", true,  kNaN, 50.0,   1.0, "SS-LF-1");  // long stop, marketable
                strategy_entry("E2", false, kNaN, 200.0,  1.0, "SS-LF-2");  // short stop, marketable
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(near(p.pos(), 0.0));                   // stays flat both before and after fix
    CHECK(p.trade_count() == 1);                 // E1 long dur-0, closed by E2
    if (p.trade_count() == 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_id == "E1");
    }
}

// ─────────────────────────────────────────────────────────────────────
// G2 — MM-*-A: both legs MARKET (E1 long, E2 short), from flat. Net already
// matches TV via the market fill path (flip_market_position_to); the fix
// touches only the PRICED-entry gate, so this stays byte-identical: net -1
// short held, one dur-0 long round trip.
// ─────────────────────────────────────────────────────────────────────
static void test_G2_mm_both_market_unchanged() {
    std::printf("test_G2_mm_both_market_unchanged\n");
    struct P : DualProbeBase {
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E1", true,  kNaN, kNaN, 1.0, "MM-LF-1");  // long market
                strategy_entry("E2", false, kNaN, kNaN, 1.0, "MM-LF-2");  // short market
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(near(p.pos(), -1.0));                  // unchanged by the fix
    CHECK(p.trade_count() == 1);
}

// ─────────────────────────────────────────────────────────────────────
// G3 — placement-REJECTED contributes 0. Capital 1000; E1 long MARKET qty
// 1000 (notional 100k ≫ equity) is rejected at signal time and never enters
// the pending queue → E2 short stop sees NO pending market sibling, opens
// from flat on its own qty. Net -1 short, no dur-0 close. Byte-stable: the
// rejected market must not lend qty to the second leg.
// ─────────────────────────────────────────────────────────────────────
static void test_G3_placement_rejected_contributes_zero() {
    std::printf("test_G3_placement_rejected_contributes_zero\n");
    struct P : DualProbeBase {
        P() : DualProbeBase(1000.0) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("E1", true,  kNaN, kNaN,  1000.0, "REJ-1");  // over-notional → rejected
                strategy_entry("E2", false, kNaN, 200.0,    1.0, "SS-2");   // short stop, affordable
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(near(p.pos(), -1.0));                  // E2 alone, from flat
    CHECK(p.trade_count() == 0);                 // nothing closed (E1 never opened)
}

int main() {
    test_R1_ms_lf_a_short_held();
    test_R2_ms_sf_a_long_held();
    test_R3_second_call_sizing_two_lot();
    test_G1_ss_lf_a_single_close_flat();
    test_G2_mm_both_market_unchanged();
    test_G3_placement_rejected_contributes_zero();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
