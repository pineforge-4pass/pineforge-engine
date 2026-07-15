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
 * R1/R2/R3 are the priced-second-leg KI-65 cases. G1/G3 pin the STOP-first and
 * placement-rejected controls. The MARKET/MARKET follow-up cases below pin the
 * remaining broker contract: gross placement admission, buy-before-sell fill
 * priority, and transaction-net execution for an admitted pair.
 *
 * Cell map (probe hours, TRUE combo under the probe's v6 float-div coverage):
 *   MS-LF-A (hh=04, 391 ev): E1 long MARKET, E2 short STOP → TV: long dur0,
 *                            short HELD.  Engine HEAD: close-only-flat.  [R1]
 *   MS-SF-A (hh=06/10, 782): E1 short MARKET, E2 long STOP → TV: net +1 long
 *                            HELD.  Engine HEAD: close-only-flat.        [R2]
 *   SS-LF-A (hh=08, 391 ev): E1 long STOP, E2 short STOP → single close,
 *                            net FLAT (BOTH match — must stay).          [G1]
 *   MM-*-A  (hh=00/02):      both MARKET, net can match through wrong rows. [G2]
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

// The new MARKET/MARKET oracle was exported with pyramiding=2. Keep the legacy
// KI-65 cells above on their pinned pyramiding=0 fixture and use this subclass
// only for the follow-up cases.
struct PendingMarketProbeBase : public DualProbeBase {
    explicit PendingMarketProbeBase(double capital = 1000.0)
        : DualProbeBase(capital) {
        pyramiding_ = 2;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
    }
    size_t pending_count() const { return pending_orders_.size(); }
    bool pending_pair_metadata_clean() const {
        for (const PendingOrder& order : pending_orders_) {
            if (order.paired_flat_market_peer_seq != 0
                || std::isfinite(order.paired_flat_market_transaction_qty)) {
                return false;
            }
        }
        return true;
    }
    bool pending_has(const char* id, bool is_long, double qty) const {
        for (const PendingOrder& order : pending_orders_) {
            if (order.id == id && order.is_long == is_long
                && near(order.qty, qty)) {
                return true;
            }
        }
        return false;
    }
    double logical_open_qty(const char* id) const {
        auto it = id_unclosed_qty_.find(id);
        return it == id_unclosed_qty_.end() ? 0.0 : it->second;
    }
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
// G2 — legacy long-first MM control: E1 long, E2 short, from flat. Buy-first
// already agrees with source order, so the pending-market follow-up retains
// the established net -1 short and one dur-0 long round trip.
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

// ──────────────────────────────────────────────────────────────────────
// MARKET/MARKET follow-up oracle (pf-probe-ki65-pending-market-affordability):
// both calls are explicit-qty, distinct-id, opposite MARKET strategy.entry
// calls placed from flat on the same ordinary (POOC=false, COOF=false) on_bar.
// The later call freezes a broker transaction of own + pending-opposite own.
// Admission costs that GROSS transaction at the signal close. If admitted,
// buys fill before sells and each fill nets its frozen transaction against the
// live position. The order's own qty remains the eventual target exposure.
// ─────────────────────────────────────────────────────────────────────

// HSF: short 25% first, long 25% second. The later buy's gross transaction is
// 50%, so both calls are admitted. TV fills the buy first: long 50%, then the
// earlier sell closes 25%, leaving long 25%. The trade list is therefore TWO
// long slices carrying E2's entry id, not a dur-0 short followed by a long.
static void test_MM_HSF_buy_first_exact_trade_decomposition() {
    std::printf("test_MM_HSF_buy_first_exact_trade_decomposition\n");
    struct P : PendingMarketProbeBase {
        size_t queued_after_signal = 0;
        bool own_qty_preserved = false;
        double ledger_after_pair = 0.0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("HSF-E1-S", false, kNaN, kNaN, 2.5, "HSF-E1-S");
                strategy_entry("HSF-E2-L", true,  kNaN, kNaN, 2.5, "HSF-E2-L");
                queued_after_signal = pending_orders_.size();
                own_qty_preserved = pending_orders_.size() == 2
                    && near(pending_orders_[0].qty, 2.5)
                    && near(pending_orders_[1].qty, 2.5);
            } else if (bar_index_ == 1) {
                ledger_after_pair = logical_open_qty("HSF-E2-L");
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(p.queued_after_signal == 2);
    CHECK(p.own_qty_preserved);
    CHECK(near(p.ledger_after_pair, 2.5));
    CHECK(near(p.pos(), 0.0));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& scratch = p.get_trade(0);
        const Trade& cleanup = p.get_trade(1);
        CHECK(scratch.is_long);
        CHECK(scratch.entry_id == "HSF-E2-L");
        CHECK(scratch.exit_id == "HSF-E1-S");
        CHECK(near(scratch.qty, 2.5));
        CHECK(scratch.entry_bar_index == 1);
        CHECK(scratch.exit_bar_index == 1);
        CHECK(cleanup.is_long);
        CHECK(cleanup.entry_id == "HSF-E2-L");
        CHECK(near(cleanup.qty, 2.5));
        CHECK(cleanup.entry_bar_index == 1);
        CHECK(cleanup.exit_bar_index == 2);
    }
}

// HLF mirror: the buy is already first. The later sell's admitted gross 50%
// transaction closes long 25% and opens short 25%.
static void test_MM_HLF_gross_sell_transaction_mirror() {
    std::printf("test_MM_HLF_gross_sell_transaction_mirror\n");
    struct P : PendingMarketProbeBase {
        size_t queued_after_signal = 0;
        bool own_qty_preserved = false;
        double ledger_after_pair = 0.0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("HLF-E1-L", true,  kNaN, kNaN, 2.5, "HLF-E1-L");
                strategy_entry("HLF-E2-S", false, kNaN, kNaN, 2.5, "HLF-E2-S");
                queued_after_signal = pending_orders_.size();
                own_qty_preserved = pending_orders_.size() == 2
                    && near(pending_orders_[0].qty, 2.5)
                    && near(pending_orders_[1].qty, 2.5);
            } else if (bar_index_ == 1) {
                ledger_after_pair = logical_open_qty("HLF-E2-S");
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(p.queued_after_signal == 2);
    CHECK(p.own_qty_preserved);
    CHECK(near(p.ledger_after_pair, 2.5));
    CHECK(near(p.pos(), 0.0));
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& scratch = p.get_trade(0);
        const Trade& cleanup = p.get_trade(1);
        CHECK(scratch.is_long);
        CHECK(scratch.entry_id == "HLF-E1-L");
        CHECK(scratch.exit_id == "HLF-E2-S");
        CHECK(near(scratch.qty, 2.5));
        CHECK(scratch.entry_bar_index == 1);
        CHECK(scratch.exit_bar_index == 1);
        CHECK(!cleanup.is_long);
        CHECK(cleanup.entry_id == "HLF-E2-S");
        CHECK(near(cleanup.qty, 2.5));
        CHECK(cleanup.entry_bar_index == 1);
        CHECK(cleanup.exit_bar_index == 2);
    }
}

// Source-interleaved brackets are load-bearing for the real Thula shape. The
// paired sell's transaction-net close must not purge pending EXIT orders while
// process_pending_orders is iterating its vector; both pair legs still produce
// the same HSF decomposition with bracket seq slots between them.
static void test_MM_HSF_interleaved_brackets_keep_fill_iteration_stable() {
    std::printf("test_MM_HSF_interleaved_brackets_keep_fill_iteration_stable\n");
    struct P : PendingMarketProbeBase {
        size_t queued_after_signal = 0;
        int candidate_market_orders = 0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("BR-E1-S", false, kNaN, kNaN, 2.5);
                strategy_exit("BR-X-LIM", "BR-E1-S", 90.0, kNaN,
                              kNaN, kNaN, kNaN, 100.0, "", 2.5);
                strategy_exit("BR-X-STP", "BR-E1-S", kNaN, 110.0,
                              kNaN, kNaN, kNaN, 100.0, "", 2.5);
                strategy_entry("BR-E2-L", true, kNaN, kNaN, 2.5);
                queued_after_signal = pending_orders_.size();
                for (const PendingOrder& order : pending_orders_) {
                    if (order.type == OrderType::MARKET
                        && order.paired_flat_market_candidate) {
                        ++candidate_market_orders;
                    }
                }
            } else if (bar_index_ == 1) {
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);

    CHECK(p.queued_after_signal == 4);
    CHECK(p.candidate_market_orders == 2);
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(p.get_trade(0).is_long);
        CHECK(p.get_trade(0).entry_id == "BR-E2-L");
        CHECK(p.get_trade(0).exit_id == "BR-E1-S");
        CHECK(near(p.get_trade(0).qty, 2.5));
        CHECK(p.get_trade(0).entry_bar_index == 1);
        CHECK(p.get_trade(0).exit_bar_index == 1);
        CHECK(p.get_trade(1).is_long);
        CHECK(p.get_trade(1).entry_id == "BR-E2-L");
        CHECK(near(p.get_trade(1).qty, 2.5));
        CHECK(p.get_trade(1).exit_bar_index == 2);
    }
}

// TSF/TLF: each own leg is 55% and independently affordable, but the later
// opposite call's own+pending transaction is 110% and is rejected at placement.
static void test_MM_tight_gross_110pct_rejects_later_leg_both_directions() {
    std::printf("test_MM_tight_gross_110pct_rejects_later_leg_both_directions\n");
    struct TSF : PendingMarketProbeBase {
        size_t queued_after_signal = 0;
        bool both_own_orders_queued = false;
        double position_after_finalization = 0.0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("TSF-E1-S", false, kNaN, kNaN, 5.5, "TSF-E1-S");
                strategy_entry("TSF-E2-L", true,  kNaN, kNaN, 5.5, "TSF-E2-L");
                queued_after_signal = pending_orders_.size();
                both_own_orders_queued = pending_orders_.size() == 2
                    && pending_has("TSF-E1-S", false, 5.5)
                    && pending_has("TSF-E2-L", true, 5.5);
            } else if (bar_index_ == 1) {
                position_after_finalization = pos();
                strategy_close_all();
            }
        }
    } tsf;
    struct TLF : PendingMarketProbeBase {
        size_t queued_after_signal = 0;
        bool both_own_orders_queued = false;
        double position_after_finalization = 0.0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("TLF-E1-L", true,  kNaN, kNaN, 5.5, "TLF-E1-L");
                strategy_entry("TLF-E2-S", false, kNaN, kNaN, 5.5, "TLF-E2-S");
                queued_after_signal = pending_orders_.size();
                both_own_orders_queued = pending_orders_.size() == 2
                    && pending_has("TLF-E1-L", true, 5.5)
                    && pending_has("TLF-E2-S", false, 5.5);
            } else if (bar_index_ == 1) {
                position_after_finalization = pos();
                strategy_close_all();
            }
        }
    } tlf;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    tsf.run(bars, 3);
    tlf.run(bars, 3);

    CHECK(tsf.queued_after_signal == 2);
    CHECK(tsf.both_own_orders_queued);
    CHECK(near(tsf.position_after_finalization, -5.5));
    CHECK(tsf.trade_count() == 1);
    if (tsf.trade_count() == 1) {
        const Trade& t = tsf.get_trade(0);
        CHECK(!t.is_long);
        CHECK(t.entry_id == "TSF-E1-S");
        CHECK(near(t.qty, 5.5));
    }
    CHECK(tlf.queued_after_signal == 2);
    CHECK(tlf.both_own_orders_queued);
    CHECK(near(tlf.position_after_finalization, 5.5));
    CHECK(tlf.trade_count() == 1);
    if (tlf.trade_count() == 1) {
        const Trade& t = tlf.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_id == "TLF-E1-L");
        CHECK(near(t.qty, 5.5));
    }
}

// Single-order controls prove that rejection above comes from pending-aware
// gross 110% admission, not from rejecting an own 55% explicit market order.
static void test_MM_tight_single_55pct_controls_admit() {
    std::printf("test_MM_tight_single_55pct_controls_admit\n");
    struct CTL : PendingMarketProbeBase {
        bool long_side;
        explicit CTL(bool side) : long_side(side) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry(long_side ? "CTL-L" : "CTL-S", long_side,
                               kNaN, kNaN, 5.5,
                               long_side ? "CTL-L" : "CTL-S");
            } else if (bar_index_ == 1) {
                strategy_close_all();
            }
        }
    } ctl(true), cts(false);
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    ctl.run(bars, 3);
    cts.run(bars, 3);

    CHECK(ctl.trade_count() == 1);
    if (ctl.trade_count() == 1) {
        CHECK(ctl.get_trade(0).is_long);
        CHECK(ctl.get_trade(0).entry_id == "CTL-L");
        CHECK(near(ctl.get_trade(0).qty, 5.5));
    }
    CHECK(cts.trade_count() == 1);
    if (cts.trade_count() == 1) {
        CHECK(!cts.get_trade(0).is_long);
        CHECK(cts.get_trade(0).entry_id == "CTL-S");
        CHECK(near(cts.get_trade(0).qty, 5.5));
    }
}

// Characterization fence: every predicate excluded from the clean-room pair
// contract must keep ordinary placement. At the 55% wedge both own legs are
// independently affordable but a leaked gross gate would reject the second.
static void test_MM_scope_predicates_do_not_pair_or_gross_gate() {
    std::printf("test_MM_scope_predicates_do_not_pair_or_gross_gate\n");
    struct P : PendingMarketProbeBase {
        enum class Mode {
            DEFAULT_QTY, SAME_ID, SAME_DIRECTION, POOC, COOF,
            RAW_SIBLING, OCA, SLIPPAGE, ZERO_QTY, NON_P2,
            CUSTOM_MARGIN, RISK_RULE, THREE_CALLS,
        };
        Mode mode;
        size_t queued = 0;
        bool metadata_clean = false;
        bool issued = false;

        explicit P(Mode m) : mode(m) {
            if (mode == Mode::DEFAULT_QTY) default_qty_value_ = 5.5;
            if (mode == Mode::POOC) process_orders_on_close_ = true;
            if (mode == Mode::COOF) calc_on_order_fills_ = true;
            if (mode == Mode::SLIPPAGE) slippage_ = 1;
            if (mode == Mode::NON_P2) pyramiding_ = 1;
            if (mode == Mode::CUSTOM_MARGIN) {
                margin_long_ = 50.0;
                margin_short_ = 50.0;
            }
            if (mode == Mode::RISK_RULE) risk_max_position_size_ = 100.0;
        }
        void snapshot() {
            queued = pending_count();
            metadata_clean = pending_pair_metadata_clean();
        }
        void on_bar(const Bar&) override {
            if (bar_index_ != 0 || issued) return;
            issued = true;
            switch (mode) {
                case Mode::DEFAULT_QTY:
                    strategy_entry("D-S", false);
                    strategy_entry("D-L", true);
                    break;
                case Mode::SAME_ID:
                    strategy_entry("SAME", false, kNaN, kNaN, 5.5);
                    strategy_entry("SAME", true, kNaN, kNaN, 5.5);
                    break;
                case Mode::SAME_DIRECTION:
                    strategy_entry("DIR-1", true, kNaN, kNaN, 5.5);
                    strategy_entry("DIR-2", true, kNaN, kNaN, 5.5);
                    break;
                case Mode::RAW_SIBLING:
                    strategy_order("RAW-S", false, 5.5);
                    strategy_entry("RAW-L", true, kNaN, kNaN, 5.5);
                    break;
                case Mode::OCA:
                    strategy_entry("OCA-S", false, kNaN, kNaN, 5.5, "",
                                   "PAIR-G", 1);
                    strategy_entry("OCA-L", true, kNaN, kNaN, 5.5, "",
                                   "PAIR-G", 1);
                    break;
                case Mode::ZERO_QTY:
                    strategy_entry("ZERO-S", false, kNaN, kNaN, 0.0);
                    strategy_entry("ZERO-L", true, kNaN, kNaN, 5.5);
                    break;
                case Mode::CUSTOM_MARGIN:
                    strategy_entry("MARGIN-S", false, kNaN, kNaN, 11.0);
                    strategy_entry("MARGIN-L", true, kNaN, kNaN, 11.0);
                    break;
                case Mode::THREE_CALLS:
                    strategy_entry("THREE-L1", true, kNaN, kNaN, 5.5);
                    strategy_entry("THREE-L2", true, kNaN, kNaN, 5.5);
                    strategy_entry("THREE-S3", false, kNaN, kNaN, 5.5);
                    break;
                case Mode::POOC:
                case Mode::COOF:
                case Mode::SLIPPAGE:
                case Mode::NON_P2:
                case Mode::RISK_RULE:
                    strategy_entry("MODE-S", false, kNaN, kNaN, 5.5);
                    strategy_entry("MODE-L", true, kNaN, kNaN, 5.5);
                    break;
            }
            snapshot();
        }
    } default_qty(P::Mode::DEFAULT_QTY), same_id(P::Mode::SAME_ID),
      same_direction(P::Mode::SAME_DIRECTION), pooc(P::Mode::POOC),
      coof(P::Mode::COOF), raw(P::Mode::RAW_SIBLING), oca(P::Mode::OCA),
      slippage(P::Mode::SLIPPAGE), zero_qty(P::Mode::ZERO_QTY),
      non_p2(P::Mode::NON_P2), custom_margin(P::Mode::CUSTOM_MARGIN),
      risk_rule(P::Mode::RISK_RULE), three_calls(P::Mode::THREE_CALLS);

    Bar one[1] = { mk(100, 600'000) };
    P* probes[] = { &default_qty, &same_id, &same_direction, &pooc, &coof,
                    &raw, &oca, &slippage, &zero_qty, &non_p2,
                    &custom_margin, &risk_rule, &three_calls };
    for (P* probe : probes) probe->run(one, 1);

    CHECK(default_qty.queued == 2 && default_qty.metadata_clean);
    CHECK(same_id.queued == 1 && same_id.metadata_clean);
    CHECK(same_direction.queued == 2 && same_direction.metadata_clean);
    CHECK(pooc.queued == 2 && pooc.metadata_clean);
    CHECK(coof.queued == 2 && coof.metadata_clean);
    CHECK(raw.queued == 2 && raw.metadata_clean);
    CHECK(oca.queued == 2 && oca.metadata_clean);
    CHECK(slippage.queued == 2 && slippage.metadata_clean);
    CHECK(zero_qty.queued == 2 && zero_qty.metadata_clean);
    CHECK(non_p2.queued == 2 && non_p2.metadata_clean);
    CHECK(custom_margin.queued == 2 && custom_margin.metadata_clean);
    CHECK(risk_rule.queued == 2 && risk_rule.metadata_clean);
    CHECK(three_calls.queued == 3 && three_calls.metadata_clean);
}

// A market order cannot remain pending across the next source evaluation: it
// fills at that bar's open before on_bar. This pins the reachable cross-bar
// shape—second placement sees a live position, not a same-source flat peer.
static void test_MM_cross_bar_calls_do_not_pair() {
    std::printf("test_MM_cross_bar_calls_do_not_pair\n");
    struct P : PendingMarketProbeBase {
        size_t queued = 0;
        bool metadata_clean = false;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("XB-S", false, kNaN, kNaN, 5.5);
            } else if (bar_index_ == 1) {
                strategy_entry("XB-L", true, kNaN, kNaN, 5.5);
                queued = pending_count();
                metadata_clean = pending_pair_metadata_clean();
            }
        }
    } p;
    Bar bars[2] = { mk(100, 600'000), mk(100, 1'200'000) };
    p.run(bars, 2);
    CHECK(p.queued == 1);
    CHECK(p.metadata_clean);
}

// Pair scope is the complete broker book, not just the candidate source bar.
// A prior-bar long limit remains resting while the current short/long MARKET
// calls are placed, then gaps through at the shared next open. All three entry-
// like orders must retain ordinary sequence semantics. At the 55% wedge, a
// leaked pair would gross-reject PAIR-L and leave the short held instead.
static void test_MM_prior_bar_gapped_limit_disqualifies_current_pair() {
    std::printf("test_MM_prior_bar_gapped_limit_disqualifies_current_pair\n");
    struct P : PendingMarketProbeBase {
        double position_after_fills = 0.0;
        int trades_after_fills = 0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("REST-L", true, 90.0, kNaN, 1.0);
            } else if (bar_index_ == 1) {
                strategy_entry("PAIR-S", false, kNaN, kNaN, 5.5);
                strategy_entry("PAIR-L", true,  kNaN, kNaN, 5.5);
            } else if (bar_index_ == 2) {
                position_after_fills = pos();
                trades_after_fills = trade_count();
                strategy_close_all();
            }
        }
    } p;
    Bar bars[4] = {
        mk(100, 600'000), mk(100, 1'200'000),
        mk(80, 1'800'000), mk(80, 2'400'000),
    };
    p.run(bars, 4);

    CHECK(near(p.position_after_fills, 5.5));
    CHECK(p.trades_after_fills == 2);
    if (p.trades_after_fills == 2) {
        CHECK(p.get_trade(0).is_long);
        CHECK(p.get_trade(0).entry_id == "REST-L");
        CHECK(!p.get_trade(1).is_long);
        CHECK(p.get_trade(1).entry_id == "PAIR-S");
    }
}

// Pair lifecycle: removing or replacing one leg must clear the survivor's
// frozen gross transaction. Otherwise an orphan could open own+removed-peer.
static void test_MM_cancel_and_replacement_unpair_survivors() {
    std::printf("test_MM_cancel_and_replacement_unpair_survivors\n");
    struct Cancel : PendingMarketProbeBase {
        size_t queued = 0;
        bool survivor_clean = false;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("CAN-S", false, kNaN, kNaN, 2.5);
                strategy_entry("CAN-L", true, kNaN, kNaN, 2.5);
                strategy_cancel("CAN-S");
                queued = pending_count();
                survivor_clean = pending_pair_metadata_clean()
                    && pending_has("CAN-L", true, 2.5);
            } else if (bar_index_ == 1) {
                strategy_close_all();
            }
        }
    } cancel;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    cancel.run(bars, 3);
    CHECK(cancel.queued == 1);
    CHECK(cancel.survivor_clean);
    CHECK(cancel.trade_count() == 1);
    if (cancel.trade_count() == 1) {
        CHECK(cancel.get_trade(0).entry_id == "CAN-L");
        CHECK(near(cancel.get_trade(0).qty, 2.5));
    }

    struct Replace : PendingMarketProbeBase {
        size_t queued = 0;
        bool both_clean = false;
        double position_after_fills = 0.0;
        int trades_after_fills = 0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("REP-S", false, kNaN, kNaN, 2.5);
                strategy_entry("REP-L", true, kNaN, kNaN, 2.5);
                strategy_entry("REP-S", false, kNaN, kNaN, 5.5);
                queued = pending_count();
                both_clean = pending_pair_metadata_clean()
                    && pending_has("REP-S", false, 5.5)
                    && pending_has("REP-L", true, 2.5);
            } else if (bar_index_ == 1) {
                position_after_fills = pos();
                trades_after_fills = trade_count();
                strategy_close_all();
            }
        }
    } replace;
    replace.run(bars, 3);
    CHECK(replace.queued == 2);
    CHECK(replace.both_clean);
    CHECK(replace.trades_after_fills == 1);
    CHECK(!replace.get_trade(0).is_long);
    CHECK(replace.get_trade(0).entry_id == "REP-S");
    CHECK(near(replace.position_after_fills, 2.5));

    struct CancelRearm : PendingMarketProbeBase {
        int trades_after_fills = 0;
        double position_after_fills = 0.0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("CR-A-L", true,  kNaN, kNaN, 2.5);
                strategy_entry("CR-B-S", false, kNaN, kNaN, 2.5);
                strategy_cancel("CR-A-L");
                strategy_entry("CR-C-L", true, kNaN, kNaN, 2.5);
            } else if (bar_index_ == 1) {
                trades_after_fills = trade_count();
                position_after_fills = pos();
                strategy_close_all();
            }
        }
    } cancel_rearm;
    cancel_rearm.run(bars, 3);
    CHECK(cancel_rearm.trades_after_fills == 1);
    CHECK(!cancel_rearm.get_trade(0).is_long);
    CHECK(cancel_rearm.get_trade(0).entry_id == "CR-B-S");
    CHECK(near(cancel_rearm.position_after_fills, 2.5));
}

// A current-bar candidate that replaces a prior-bar entry is a mutation, not
// one side of an exact two-call pair. A rests as a limit on bar 0; on bar 1 B
// is called first and A is replaced by a MARKET candidate. Because A preserves
// its older broker sequence, leaked finalization would gross-reject B at 110%
// and leave A long. The tainted ordinary path fills A then B and holds B short.
static void test_MM_prior_bar_entry_replacement_taints_current_pair_set() {
    std::printf(
        "test_MM_prior_bar_entry_replacement_taints_current_pair_set\n");
    struct P : PendingMarketProbeBase {
        double position_after_fills = 0.0;
        int trades_after_fills = 0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("REP-A", true, 90.0, kNaN, 1.0);
            } else if (bar_index_ == 1) {
                strategy_entry("REP-B", false, kNaN, kNaN, 5.5);
                strategy_entry("REP-A", true,  kNaN, kNaN, 5.5);
            } else if (bar_index_ == 2) {
                position_after_fills = pos();
                trades_after_fills = trade_count();
                strategy_close_all();
            }
        }
    } p;
    Bar bars[4] = {
        mk(100, 600'000), mk(100, 1'200'000),
        mk(100, 1'800'000), mk(100, 2'400'000),
    };
    p.run(bars, 4);

    CHECK(near(p.position_after_fills, -5.5));
    CHECK(p.trades_after_fills == 1);
    if (p.trades_after_fills == 1) {
        CHECK(p.get_trade(0).is_long);
        CHECK(p.get_trade(0).entry_id == "REP-A");
        CHECK(p.get_trade(0).exit_id == "REP-B");
        CHECK(near(p.get_trade(0).qty, 5.5));
    }
}

// Pair finalization must see the COMPLETE source-bar set. Alternating triples
// are ordinary source-ordered calls; call 2 must neither pair nor gross-reject
// before call 3 is known.
static void test_MM_alternating_three_call_sets_remain_ordinary() {
    std::printf("test_MM_alternating_three_call_sets_remain_ordinary\n");
    struct P : PendingMarketProbeBase {
        bool short_first;
        size_t queued_after_signal = 0;
        double position_after_fills = 0.0;
        int trades_after_fills = 0;
        explicit P(bool sf) : short_first(sf) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                if (short_first) {
                    strategy_entry("SLS-1-S", false, kNaN, kNaN, 5.5);
                    strategy_entry("SLS-2-L", true,  kNaN, kNaN, 5.5);
                    strategy_entry("SLS-3-S", false, kNaN, kNaN, 5.5);
                } else {
                    strategy_entry("LSL-1-L", true,  kNaN, kNaN, 5.5);
                    strategy_entry("LSL-2-S", false, kNaN, kNaN, 5.5);
                    strategy_entry("LSL-3-L", true,  kNaN, kNaN, 5.5);
                }
                queued_after_signal = pending_count();
            } else if (bar_index_ == 1) {
                position_after_fills = pos();
                trades_after_fills = trade_count();
                strategy_close_all();
            }
        }
    } sls(true), lsl(false);
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    sls.run(bars, 3);
    lsl.run(bars, 3);

    CHECK(sls.queued_after_signal == 3);
    CHECK(sls.trades_after_fills == 2);
    CHECK(near(sls.position_after_fills, -5.5));
    CHECK(sls.get_trade(0).entry_id == "SLS-1-S");
    CHECK(sls.get_trade(1).entry_id == "SLS-2-L");
    CHECK(sls.trade_count() == 3);

    CHECK(lsl.queued_after_signal == 3);
    CHECK(lsl.trades_after_fills == 2);
    CHECK(near(lsl.position_after_fills, 5.5));
    CHECK(lsl.get_trade(0).entry_id == "LSL-1-L");
    CHECK(lsl.get_trade(1).entry_id == "LSL-2-S");
    CHECK(lsl.trade_count() == 3);
}

// A scope change after placement but before broker processing must suppress
// finalization. This pins the fill-boundary risk-config revalidation.
static void test_MM_pair_scope_revalidated_before_fill() {
    std::printf("test_MM_pair_scope_revalidated_before_fill\n");
    struct P : PendingMarketProbeBase {
        size_t queued_after_signal = 0;
        double position_after_fills = 0.0;
        int trades_after_fills = 0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("RISK-S", false, kNaN, kNaN, 2.5);
                strategy_entry("RISK-L", true,  kNaN, kNaN, 2.5);
                queued_after_signal = pending_count();
                // Non-blocking at qty 2.5, but its mere configuration is out
                // of the pinned pair scope and must be observed at finalization.
                risk_max_position_size_ = 100.0;
            } else if (bar_index_ == 1) {
                position_after_fills = pos();
                trades_after_fills = trade_count();
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);
    CHECK(p.queued_after_signal == 2);
    CHECK(p.trades_after_fills == 1);
    CHECK(!p.get_trade(0).is_long);
    CHECK(p.get_trade(0).entry_id == "RISK-S");
    CHECK(near(p.position_after_fills, 2.5));
}

// Pair recognition counts every same-bar flat entry-like broker order, not
// only explicit candidates. A default/OCA/RAW/priced third order disqualifies
// the bar and leaves the first two in ordinary source order.
static void test_MM_mixed_third_entry_like_order_disqualifies_pair() {
    std::printf("test_MM_mixed_third_entry_like_order_disqualifies_pair\n");
    struct P : PendingMarketProbeBase {
        enum class Third { DEFAULT_MARKET, OCA_MARKET, RAW_MARKET, PRICED };
        Third third;
        int trades_after_fills = 0;
        bool first_trade_is_ordinary_short = false;
        explicit P(Third t) : third(t) {}
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("MIX-E1-S", false, kNaN, kNaN, 2.5);
                strategy_entry("MIX-E2-L", true,  kNaN, kNaN, 2.5);
                switch (third) {
                    case Third::DEFAULT_MARKET:
                        strategy_entry("MIX-D3-S", false);
                        break;
                    case Third::OCA_MARKET:
                        strategy_entry("MIX-O3-S", false, kNaN, kNaN, 1.0,
                                       "", "MIX-G", 1);
                        break;
                    case Third::RAW_MARKET:
                        strategy_order("MIX-R3-S", false, 1.0);
                        break;
                    case Third::PRICED:
                        strategy_entry("MIX-P3-S", false, kNaN, 50.0, 1.0);
                        break;
                }
            } else if (bar_index_ == 1) {
                trades_after_fills = trade_count();
                if (trades_after_fills > 0) {
                    first_trade_is_ordinary_short =
                        !get_trade(0).is_long
                        && get_trade(0).entry_id == "MIX-E1-S";
                }
                strategy_cancel_all();
                strategy_close_all();
            }
        }
    } def(P::Third::DEFAULT_MARKET), oca(P::Third::OCA_MARKET),
      raw(P::Third::RAW_MARKET), priced(P::Third::PRICED);
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    P* probes[] = { &def, &oca, &raw, &priced };
    for (P* probe : probes) probe->run(bars, 3);
    for (P* probe : probes) {
        CHECK(probe->trades_after_fills >= 1);
        CHECK(probe->first_trade_is_ordinary_short);
    }
}

// Quantize each own leg once, then sum those frozen broker quantities. Raw
// 5.1+5.1 would be 102% and reject; with qty_step=1 TV sends 5+5=100%, admits,
// and the internal pre-quantized path must preserve exactly 5 contracts.
static void test_MM_pair_uses_sum_of_frozen_quantized_own_qty() {
    std::printf("test_MM_pair_uses_sum_of_frozen_quantized_own_qty\n");
    struct P : PendingMarketProbeBase {
        double position_after_pair = 0.0;
        double ledger_after_pair = 0.0;
        P() { qty_step_ = 1.0; }
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("Q-S", false, kNaN, kNaN, 5.1);
                strategy_entry("Q-L", true,  kNaN, kNaN, 5.1);
            } else if (bar_index_ == 1) {
                position_after_pair = pos();
                ledger_after_pair = logical_open_qty("Q-L");
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);
    CHECK(near(p.position_after_pair, 5.0));
    CHECK(near(p.ledger_after_pair, 5.0));
    CHECK(p.trade_count() == 2);
    CHECK(p.get_trade(0).is_long);
    CHECK(p.get_trade(0).entry_id == "Q-L");
    CHECK(near(p.get_trade(0).qty, 5.0));
    CHECK(near(p.get_trade(1).qty, 5.0));
}

// The explicit-qty adverse-gap recheck must cost the paired GROSS broker fill.
// Own qty 4 costs only 520 at the 130 fill and would admit incorrectly; gross
// qty 8 costs 1040, so the buy is declined and the surviving short fills own 4.
static void test_MM_pair_fill_gap_gate_uses_gross_transaction_qty() {
    std::printf("test_MM_pair_fill_gap_gate_uses_gross_transaction_qty\n");
    struct P : PendingMarketProbeBase {
        double position_after_gap = 0.0;
        int trades_after_gap = 0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("GAP-S", false, kNaN, kNaN, 4.0);
                strategy_exit("GAP-X1", "GAP-L", 180.0, kNaN,
                              kNaN, kNaN, kNaN, 50.0, "", 2.0);
                strategy_exit("GAP-X2", "GAP-L", kNaN, 80.0,
                              kNaN, kNaN, kNaN, 50.0, "", 2.0);
                strategy_entry("GAP-L", true,  kNaN, kNaN, 4.0);
            } else if (bar_index_ == 1) {
                position_after_gap = pos();
                trades_after_gap = trade_count();
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(130, 1'200'000), mk(130, 1'800'000) };
    p.run(bars, 3);
    CHECK(near(p.position_after_gap, -4.0));
    CHECK(p.trades_after_gap == 0);
    CHECK(p.trade_count() == 1);
    CHECK(!p.get_trade(0).is_long);
    CHECK(p.get_trade(0).entry_id == "GAP-S");
    CHECK(near(p.get_trade(0).qty, 4.0));
}

// Deferred percent-layered exits armed between pair calls resolve against the
// final own exposure, not the transient gross open. At 2.5, 40%/60% must freeze
// to 1.0/1.5 only after transaction netting completes.
static void test_MM_pair_defers_percent_exit_reconciliation_until_net() {
    std::printf("test_MM_pair_defers_percent_exit_reconciliation_until_net\n");
    struct P : PendingMarketProbeBase {
        double position_after_pair = 0.0;
        double ledger_after_pair = 0.0;
        double exit_one_qty = 0.0;
        double exit_two_qty = 0.0;
        void on_bar(const Bar&) override {
            if (bar_index_ == 0) {
                strategy_entry("LAY-S", false, kNaN, kNaN, 2.5);
                strategy_exit("LAY-X1", "LAY-L", 150.0, kNaN,
                              kNaN, kNaN, kNaN, 40.0, "", kNaN);
                strategy_exit("LAY-X2", "LAY-L", 160.0, kNaN,
                              kNaN, kNaN, kNaN, 60.0, "", kNaN);
                strategy_entry("LAY-L", true, kNaN, kNaN, 2.5);
            } else if (bar_index_ == 1) {
                position_after_pair = pos();
                ledger_after_pair = logical_open_qty("LAY-L");
                for (const PendingOrder& order : pending_orders_) {
                    if (order.id == "LAY-X1") exit_one_qty = order.qty;
                    if (order.id == "LAY-X2") exit_two_qty = order.qty;
                }
                strategy_cancel_all();
                strategy_close_all();
            }
        }
    } p;
    Bar bars[3] = { mk(100, 600'000), mk(100, 1'200'000), mk(100, 1'800'000) };
    p.run(bars, 3);
    CHECK(near(p.position_after_pair, 2.5));
    CHECK(near(p.ledger_after_pair, 2.5));
    CHECK(near(p.exit_one_qty, 1.0));
    CHECK(near(p.exit_two_qty, 1.5));
    CHECK(p.trade_count() == 2);
}

int main() {
    test_R1_ms_lf_a_short_held();
    test_R2_ms_sf_a_long_held();
    test_R3_second_call_sizing_two_lot();
    test_G1_ss_lf_a_single_close_flat();
    test_G2_mm_both_market_unchanged();
    test_G3_placement_rejected_contributes_zero();
    test_MM_HSF_buy_first_exact_trade_decomposition();
    test_MM_HLF_gross_sell_transaction_mirror();
    test_MM_HSF_interleaved_brackets_keep_fill_iteration_stable();
    test_MM_tight_gross_110pct_rejects_later_leg_both_directions();
    test_MM_tight_single_55pct_controls_admit();
    test_MM_scope_predicates_do_not_pair_or_gross_gate();
    test_MM_cross_bar_calls_do_not_pair();
    test_MM_prior_bar_gapped_limit_disqualifies_current_pair();
    test_MM_cancel_and_replacement_unpair_survivors();
    test_MM_prior_bar_entry_replacement_taints_current_pair_set();
    test_MM_alternating_three_call_sets_remain_ordinary();
    test_MM_pair_scope_revalidated_before_fill();
    test_MM_mixed_third_entry_like_order_disqualifies_pair();
    test_MM_pair_uses_sum_of_frozen_quantized_own_qty();
    test_MM_pair_fill_gap_gate_uses_gross_transaction_qty();
    test_MM_pair_defers_percent_exit_reconciliation_until_net();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
