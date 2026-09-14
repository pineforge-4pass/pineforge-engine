/*
 * test_close_id_retires_ledger.cpp — a sole default-FIFO strategy.close(id)
 * retires the id's WHOLE unclosed-entry ledger, even when availability
 * capped the qty it could fill (round-4b finding F1).
 *
 * The engine keeps a logical ledger id_unclosed_qty_[id] — the qty entered
 * under `id` and not yet targeted by a close(id) — credited on every entry
 * fill under the id and consumed by a default-FIFO strategy.close(id) (no
 * qty / qty_percent, FIFO close-entries rule). Under process_orders_on_close
 * such a close resolves its target at the bar-close flush as
 * min(ledger, avail), avail = position minus the other ids' standing close
 * reservations (flush_active_same_bar_close, engine_strategy_commands.cpp).
 *
 * Bug (pre-fix): when avail capped the target the flush debited only the
 * target and CARRIED the remainder; the next entry under the same id then
 * re-credited on top of the carry (id_unclosed_qty_[id] += qty on the fill),
 * so the next close(id) over-closed by exactly the carry. TradingView
 * retires every entry under the id on a close(id) ("all entries with the ID
 * are exited at once"): a capped fill still retires the id.
 *
 * Evidence (3commas grid bots on BINANCE:ETHUSDT.P-family tapes, round 4b):
 * xlm-grid 2025-05-08 10:45 TP_L35 closes 0.0987 of an L35 lot of 0.1043 on
 * BOTH engine and TV; on 2026-02-06 14:30 the engine then closed
 * 0.1099 = 0.1043 + 0.0056 carried while TV closed 0.1043 (nvdax: engine
 * 0.0972 vs TV 0.0926). A Python port of the two ledger rules reproduced the
 * engine's closed qty byte-for-byte on 3/3 bots.
 *
 * The cap in these pins comes from the same mechanism the bots hit: a
 * standing close reservation on another id, written when two close(id)
 * calls of one source site collapse into one surviving fill on the same bar
 * (the same-callsite replacement rule) and released only by a later sole
 * call for that id or by going flat. That reservation never decaying is a
 * separate open item (round-4b F2, TV rule unpinned); the pins only need it
 * as the source of a capped avail. If F2 changes, re-source the cap.
 *
 * Pins:
 *   A. POOC, sole close(L35) capped 0.1043 -> 0.0987: the L35 ledger is
 *      retired (absent), the fill is still 0.0987, the position stays open.
 *      Re-entry L35 0.1043, then close(L35) closes exactly 0.1043 (pre-fix
 *      0.1099 = 0.1043 + 0.0056 carried).
 *   B. A DELIBERATE partial (explicit qty) keeps its remainder: it never
 *      touches the ledger, so the id stays closable and a later default
 *      close(L35) still resolves the ledger, not the partial's remainder
 *      (pre-existing explicit-qty semantics, unchanged by F1).
 *   C. The next-bar (deferred) path: the ledger is retired whole at call
 *      time as well, so after a declined-reversal suppression
 *      (suppress_declined_reversal_close_legs) the exact pre-call balance
 *      comes back — the re-credit restores target + retired remainder, not
 *      just the target — and the follow-up close(id) closes the whole
 *      ledger. Dropping the retired term leaves the ledger at the target.
 *   D. The COOF fill-recalc path under POOC (the strategy_close immediate
 *      re-credit): a close(id) issued inside a bar-close fill recalc is
 *      materialized as an expiring instruction and its ledger comes back
 *      whole at once (target + retired), so the ordinary close that follows
 *      resolves the full ledger.
 *   E. The one cap F1 does NOT retire on: a tokenized sole call whose
 *      shortfall comes SOLELY from this bar's other pending callsites
 *      (site1 close(A) admitted first, site2 close(B) capped by it) keeps
 *      the pre-F1 debit-by-target carry. That case is unpinned against TV
 *      and its zero-target sibling (the fresh A/A->B oracle in
 *      enqueue_same_bar_close) performs no cleanup, so a partial same-bar
 *      cap must not diverge from a total one. The cap in every F1 evidence
 *      point is a position / prior-bar reservation cap (A, A2, C, D).
 */

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int64_t kStep = 15 * 60 * 1000;

Bar flat_bar(int i, double px) {
    Bar b;
    b.open = px; b.high = px + 0.5; b.low = px - 0.5; b.close = px;
    b.volume = 1000.0; b.timestamp = (int64_t)(i + 1) * kStep;
    return b;
}

// Scripted probe: the test hands it one action per bar index. Grid-bot
// shape: many lots under distinct ids, explicit qty per entry, no costs.
class Probe : public pineforge::source::PineStrategyHost {
public:
    explicit Probe(bool pooc) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        pyramiding_ = 20;
        margin_call_enabled_ = false;
        process_orders_on_close_ = pooc;
        set_syminfo_mintick(0.0001);
    }
    // Pin C's shape (test_declined_reversal_close_leg.cpp): all-in
    // percent-of-equity so the KI-54/KI-57 reversal-admission gate declines
    // an opposite MARKET entry whose fill bar opens one mintick above the
    // signal close (qty_step 0).
    void all_in_percent_of_equity() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        pyramiding_ = 2;
        set_syminfo_mintick(0.01);
    }
    void enable_calc_on_order_fills() { calc_on_order_fills_ = true; }
    bool at_close_cursor() const { return coof_cursor_is_bar_close_; }
    void entry_stop(const std::string& id, double stop, double qty) {
        strategy_entry(id, /*is_long=*/true, kNaN, stop, qty);
    }
    // A stop exit over the whole position (no from_entry) for an explicit qty.
    void exit_stop_qty(const std::string& id, double stop, double qty) {
        strategy_exit(id, "", kNaN, stop, kNaN, kNaN, kNaN, kNaN, "", qty);
    }
    int recalc_calls = 0;
    int close_cursor_recalc_calls = 0;
    std::vector<std::function<void(Probe&)>> steps;
    // Runs on the recalc passes of a bar only (COOF); `steps` run on the
    // ordinary pass only.
    std::vector<std::function<void(Probe&)>> recalc_steps;
    void on_source_bar(const Bar&) override {
        if (bar_index_ < 0) return;
        if (coof_fill_recalc_active_) {
            if (bar_index_ < (int)recalc_steps.size() && recalc_steps[bar_index_])
                recalc_steps[bar_index_](*this);
            return;
        }
        if (bar_index_ < (int)steps.size() && steps[bar_index_])
            steps[bar_index_](*this);
    }
    void entry(const std::string& id, double qty) {
        strategy_entry(id, /*is_long=*/true, kNaN, kNaN, qty);
    }
    void entry_default(const std::string& id, bool is_long) {
        strategy_entry(id, is_long);
    }
    void close(const std::string& id) { strategy_close(id); }
    void close_at_site(const std::string& id, uint64_t token) {
        strategy_close(id, "", kNaN, kNaN, false, token);
    }
    void close_qty(const std::string& id, double qty) {
        strategy_close(id, "", qty);
    }
    // Ledger / reservation readers (protected on BacktestEngine).
    bool ledger_has(const std::string& id) const {
        return id_unclosed_qty_.count(id) != 0;
    }
    double ledger(const std::string& id) const {
        auto it = id_unclosed_qty_.find(id);
        return it == id_unclosed_qty_.end() ? 0.0 : it->second;
    }
    double reserved(const std::string& id) const {
        auto it = close_reserved_qty_.find(id);
        return it == close_reserved_qty_.end() ? 0.0 : it->second;
    }
    double site_reserved(uint64_t token, const std::string& id) const {
        auto owner = callsite_close_reserved_qty_.find(token);
        if (owner == callsite_close_reserved_qty_.end()) return 0.0;
        auto it = owner->second.find(id);
        return it == owner->second.end() ? 0.0 : it->second;
    }
    bool is_long() const { return position_side_ == PositionSide::LONG; }
    double pos() const { return position_qty_; }
    bool is_flat() const { return position_side_ == PositionSide::FLAT; }
    const std::vector<Trade>& all_trades() const { return trades_; }
    // Records of the ledger / position AFTER each bar's broker point.
    std::vector<double> ledger_l35_after_bar;
    std::vector<double> pos_after_bar;
    // Pin D: ledger / position observed inside a COOF fill recalc.
    std::vector<double> recalc_ledger_l;
    std::vector<double> recalc_pos;
};

// Sum of closed-trade qty carrying this exit id (a FIFO close spans lots).
double closed_qty_by_exit(const Probe& p, const std::string& exit_id,
                          int exit_bar) {
    double sum = 0.0;
    for (const Trade& t : p.all_trades())
        if (t.exit_id == exit_id && t.exit_bar_index == exit_bar) sum += t.qty;
    return sum;
}

}  // namespace

// A. POOC: a capped sole close(L35) retires the whole L35 ledger.
static void test_pooc_capped_close_retires_ledger() {
    std::printf("-- A: POOC capped close(L35) retires the L35 ledger --\n");
    Probe p(/*pooc=*/true);
    // bar0: three lots — L37 0.02 (oldest), L36 0.0256, L35 0.1043 -> 0.1499.
    // bar1: close(L37); close(L36) on one bar collapse into the surviving
    //       close(L36): fills 0.0256 (FIFO: all of L37, 0.0056 of L36),
    //       position 0.1243, and leaves a standing 0.0256 reservation on L36.
    // bar2: sole close(L35): avail = 0.1243 - 0.0256 = 0.0987 < 0.1043 ledger
    //       -> fills 0.0987 (the xlm-grid TP_L35 shape), position 0.0256.
    // bar3: re-entry L35 0.1043 (+ L38 0.1 so avail is not the cap again):
    //       ledger L35 must read 0.1043, not 0.1043 + 0.0056 carried.
    // bar4: close(L35) -> closes exactly 0.1043 (pre-fix 0.1099).
    p.steps = {
        [](Probe& e) { e.entry("L37", 0.02); e.entry("L36", 0.0256); e.entry("L35", 0.1043); },
        [](Probe& e) { e.close("L37"); e.close("L36"); },
        [](Probe& e) { e.close("L35"); },
        [](Probe& e) { e.entry("L35", 0.1043); e.entry("L38", 0.1); },
        [](Probe& e) { e.close("L35"); },
        nullptr,
    };
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) bars.push_back(flat_bar(i, 100.0));
    p.run(bars.data(), (int)bars.size());

    // The capped fill itself is unchanged: 0.0987 on bar 2.
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L35", 2), 0.0987, 1e-9);
    // The load-bearing pin: the re-entered id closes exactly its new lot.
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L35", 4), 0.1043, 1e-9);   // pre-fix 0.1099
    // The position never went flat (the ledger was not cleared by a flatten).
    CHECK(!p.is_flat());
    CHECK_NEAR(p.pos(), 0.0256 + 0.1, 1e-9);
    // After the final close the L35 ledger is retired again.
    CHECK(!p.ledger_has("L35"));
    std::printf("   pos=%.6f reserved(L36)=%.6f ledger(L35)=%.6f ledger(L36)=%.6f\n",
                p.pos(), p.reserved("L36"), p.ledger("L35"), p.ledger("L36"));
    for (const Trade& t : p.all_trades())
        std::printf("   trade entry=%s exit=%s qty=%.6f exit_bar=%d\n",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.qty, t.exit_bar_index);
}

// A2. Same tape, observed at the bar the cap happens: the ledger is absent
//     right after the capped fill (pre-fix it held the 0.0056 carry).
static void test_pooc_ledger_absent_right_after_capped_fill() {
    std::printf("-- A2: the L35 ledger is absent right after the capped fill --\n");
    Probe p(/*pooc=*/true);
    p.steps = {
        [](Probe& e) { e.entry("L37", 0.02); e.entry("L36", 0.0256); e.entry("L35", 0.1043); },
        [](Probe& e) { e.close("L37"); e.close("L36"); },
        [](Probe& e) { e.close("L35"); },
        [](Probe& e) {
            // bar 3, before any new entry: what bar 2's flush left behind.
            e.ledger_l35_after_bar.push_back(e.ledger("L35"));
            e.pos_after_bar.push_back(e.pos());
        },
    };
    std::vector<Bar> bars;
    for (int i = 0; i < 4; ++i) bars.push_back(flat_bar(i, 100.0));
    p.run(bars.data(), (int)bars.size());
    CHECK(p.ledger_l35_after_bar.size() == 1);
    if (!p.ledger_l35_after_bar.empty()) {
        CHECK_NEAR(p.ledger_l35_after_bar[0], 0.0, 1e-12);       // pre-fix 0.0056
        CHECK_NEAR(p.pos_after_bar[0], 0.0256, 1e-9);
    }
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L35", 2), 0.0987, 1e-9);
    CHECK_NEAR(p.reserved("L36"), 0.0256, 1e-9);   // the F2 claim that capped avail
}

// B. A deliberate partial keeps its remainder (explicit qty never touches
//    the ledger; the id stays closable by a later default close).
static void test_deliberate_partial_keeps_remainder() {
    std::printf("-- B: a deliberate partial close(L35, qty) keeps its remainder --\n");
    for (int pooc = 0; pooc <= 1; ++pooc) {
        Probe p(pooc != 0);
        p.steps = {
            [](Probe& e) { e.entry("L36", 0.05); e.entry("L35", 0.1043); },
            [](Probe& e) { e.close_qty("L35", 0.04); },
            [](Probe& e) {
                e.ledger_l35_after_bar.push_back(e.ledger("L35"));
                e.pos_after_bar.push_back(e.pos());
            },
            [](Probe& e) { e.close("L35"); },
            nullptr, nullptr,
        };
        std::vector<Bar> bars;
        for (int i = 0; i < 6; ++i) bars.push_back(flat_bar(i, 100.0));
        p.run(bars.data(), (int)bars.size());
        const int fill_bar_partial = pooc ? 1 : 2;
        const int fill_bar_full = pooc ? 3 : 4;
        CHECK_NEAR(closed_qty_by_exit(p, "__close__L35", fill_bar_partial), 0.04, 1e-9);
        CHECK(p.ledger_l35_after_bar.size() == 1);
        if (!p.ledger_l35_after_bar.empty()) {
            // The explicit-qty close left the L35 ledger whole: the id is
            // still closable for its full entered qty.
            CHECK_NEAR(p.ledger_l35_after_bar[0], 0.1043, 1e-12);
            CHECK_NEAR(p.pos_after_bar[0], 0.1543 - 0.04, 1e-9);
        }
        // The later default close(L35) resolves the ledger (0.1043), capped
        // by the position (0.1143) — not the partial's 0.0643 remainder and
        // not retired by the partial. Pre-existing semantics, unchanged.
        CHECK_NEAR(closed_qty_by_exit(p, "__close__L35", fill_bar_full), 0.1043, 1e-9);
        CHECK(!p.ledger_has("L35"));
        std::printf("   pooc=%d pos=%.6f\n", pooc, p.pos());
    }
}

// C. Deferred (next-bar) path: the call retires the ledger whole; a
//    declined-reversal suppression restores the exact pre-call balance
//    (target + retired), and the follow-up close(id) closes the whole ledger.
static void test_deferred_close_suppression_recredits_retired_remainder() {
    std::printf("-- C: deferred close(L) retires whole; suppression re-credits target + retired --\n");
    Probe p(/*pooc=*/false);
    p.all_in_percent_of_equity();
    // bar0 (close 100): entry L all-in -> fills bar1 open @100, qty 100.
    // bar1: close(L, qty=40) explicit -> fills bar2 open: position 60,
    //       ledger L stays 100 (Pin B: an explicit qty never touches it).
    // bar2 (signal, close 110): entry S (reversal, created FIRST), then
    //       close(L): target = min(ledger 100, position 60) = 60 = full
    //       position -> a suppressible FULL close; retired remainder 40; the
    //       L ledger is erased at call time.
    // bar3 opens 111 (+1 mintick): S declines, the co-queued close(L) is
    //       suppressed and the ledger re-credited: 60 + 40 = 100 (without
    //       the retired term: 60). Position holds LONG 60.
    // bar3: entry M 50 (fixed qty, a different id) -> fills bar4 open:
    //       position 110; ledger L untouched. Bar3 closes at 79 so the add is
    //       affordable as held + add (design-market-entry-affordability):
    //       MTM 10,000 - 60*21 = 8,740 >= 110 * 79 = 8,690. (At the former
    //       close of 111 the all-in position had no free equity and TV's
    //       rule drops the add.)
    // bar4: close(L): target = min(ledger, 110) = 100 with the full
    //       re-credit (60 without it) -> fills bar5 open.
    p.steps = {
        [](Probe& e) { e.entry_default("L", true); },
        [](Probe& e) { e.close_qty("L", 40.0); },
        [](Probe& e) { e.entry_default("S", false); e.close("L"); },
        [](Probe& e) {
            e.ledger_l35_after_bar.push_back(e.ledger("L"));   // bar3, pre-entry
            e.pos_after_bar.push_back(e.pos());
            e.entry("M", 50.0);
        },
        [](Probe& e) { e.close("L"); },
        nullptr, nullptr,
    };
    auto mk = [](int i, double o, double h, double l, double c) {
        Bar b; b.open = o; b.high = h; b.low = l; b.close = c;
        b.volume = 1.0; b.timestamp = (int64_t)(i + 1) * kStep; return b;
    };
    std::vector<Bar> bars = {
        mk(0, 100, 100, 100, 100),   // bar0: place L
        mk(1, 100, 100, 100, 100),   // bar1: L fills @100; explicit partial placed
        mk(2, 100, 112,  99, 110),   // bar2: partial fills @100; S + close(L) placed
        mk(3, 111, 112,  79,  79),   // bar3: S declines (+1 tick), close suppressed
        mk(4,  79,  79,  79,  79),   // bar4: M fills; close(L) placed
        mk(5,  79,  79,  79,  79),   // bar5: close(L) fills
        mk(6,  79,  79,  79,  79),
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.ledger_l35_after_bar.size() == 1);
    if (!p.ledger_l35_after_bar.empty()) {
        // The reversal was declined and the close suppressed: LONG 60 held.
        CHECK_NEAR(p.pos_after_bar[0], 60.0, 1e-9);
        // The load-bearing pin: the exact pre-call balance is back.
        CHECK_NEAR(p.ledger_l35_after_bar[0], 100.0, 1e-9);   // 60 without the retired term
    }
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L", 2), 40.0, 1e-9);     // the explicit partial
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L", 3), 0.0, 1e-9);      // suppressed
    CHECK_NEAR(closed_qty_by_exit(p, "__close__L", 5), 100.0, 1e-9);    // 60 without the retired term
    CHECK(p.is_long());
    CHECK_NEAR(p.pos(), 10.0, 1e-9);                                    // 50 without the retired term
    CHECK(!p.ledger_has("L"));
    std::printf("   pos=%.6f ledger(L)=%.6f ledger(M)=%.6f\n",
                p.pos(), p.ledger("L"), p.ledger("M"));
    for (const Trade& t : p.all_trades())
        std::printf("   trade entry=%s exit=%s qty=%.6f exit_bar=%d\n",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.qty, t.exit_bar_index);
}

// D. COOF fill-recalc path under POOC: a close(B) issued inside a bar-close
//    (C-cursor) fill recalc is materialized as an expiring instruction and
//    its ledger is re-credited at once — target + retired — so the ordinary
//    close that follows resolves the whole ledger. The C-cursor recalc is
//    reached the way test_calc_on_order_fills.cpp's
//    PoocCloseAtCRecalcProbe reaches it: a stop entry fills mid-bar on the
//    L->H leg and the exit its recalc places reaches its exact stop at C on
//    the H->C leg, triggering a recalc whose cursor is the bar close.
static void test_coof_recalc_close_recredits_retired_remainder() {
    std::printf("-- D: COOF C-cursor recalc close(B) under POOC re-credits target + retired --\n");
    Probe p(/*pooc=*/true);
    p.enable_calc_on_order_fills();
    // bar0 (90/95/85/90): entry B market 1.0 (fills at bar0's C under POOC;
    //   ledger B 1.0) and entry A stop 105, qty 1.0.
    // bar1 (100/110/90/100; path O->L->H->C): A fills at 105 on L->H ->
    //   recalc #1 (cursor 105, in-flight leg L->H, position 2.0): exit X,
    //   stop 100, qty 1.4 over the whole position. Its level is below the
    //   L->H remainder, and the subsequent leg H->C (110->100) crosses it
    //   exactly at C: ONE fill of 1.4 (FIFO: all of B's lot, 0.4 of A's),
    //   position 0.6 -> recalc #2 at the C cursor. The logical B ledger is
    //   still 1.0 (an exit leg never touches it; close(id) reads the
    //   ledger, not the physical lots). In THAT recalc, close(B): target =
    //   min(1.0, 0.6) = 0.6, retired 0.4; the immediate re-credit restores
    //   1.0 (0.6 without the retired term); the instruction itself is a
    //   post-C POOC market order and expires.
    // bar2: read the ledger; entry N 1.0 -> fills at bar2 close: position 1.6.
    // bar3: close(B) -> closes 1.0 (0.6 without the retired term).
    p.steps = {
        [](Probe& e) { e.entry("B", 1.0); e.entry_stop("A", 105.0, 1.0); },
        nullptr,
        [](Probe& e) {
            e.ledger_l35_after_bar.push_back(e.ledger("B"));   // bar2, pre-entry
            e.pos_after_bar.push_back(e.pos());
            e.entry("N", 1.0);
        },
        [](Probe& e) { e.close("B"); },
        nullptr, nullptr,
    };
    p.recalc_steps = {
        nullptr,
        [](Probe& e) {
            ++e.recalc_calls;
            if (!e.at_close_cursor()) {
                if (e.recalc_calls == 1) e.exit_stop_qty("X", 100.0, 1.4);
                return;
            }
            ++e.close_cursor_recalc_calls;
            if (e.close_cursor_recalc_calls != 1) return;
            e.recalc_ledger_l.push_back(e.ledger("B"));   // before the call
            e.recalc_pos.push_back(e.pos());
            e.close("B");
            e.recalc_ledger_l.push_back(e.ledger("B"));   // after: re-credited at once
        },
        nullptr, nullptr, nullptr, nullptr,
    };
    auto mk = [](int i, double o, double h, double l, double c) {
        Bar b; b.open = o; b.high = h; b.low = l; b.close = c;
        b.volume = 1.0; b.timestamp = (int64_t)(i + 1) * kStep; return b;
    };
    std::vector<Bar> bars = {
        mk(0,  90,  95,  85,  90),
        mk(1, 100, 110,  90, 100),
        mk(2, 100, 100, 100, 100),
        mk(3, 100, 100, 100, 100),
        mk(4, 100, 100, 100, 100),
        mk(5, 100, 100, 100, 100),
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.last_error().empty());
    // The C-cursor recalc ran once, at position 0.6 with the B ledger at
    // 1.0, and its close(B) left the ledger at its exact pre-call balance.
    CHECK(p.recalc_calls == 2);
    CHECK(p.close_cursor_recalc_calls == 1);
    CHECK(p.recalc_ledger_l.size() == 2);
    if (p.recalc_ledger_l.size() == 2) {
        CHECK_NEAR(p.recalc_pos[0], 0.6, 1e-9);
        CHECK_NEAR(p.recalc_ledger_l[0], 1.0, 1e-9);
        CHECK_NEAR(p.recalc_ledger_l[1], 1.0, 1e-9);          // 0.6 without the retired term
    }
    CHECK(p.ledger_l35_after_bar.size() == 1);
    if (!p.ledger_l35_after_bar.empty()) {
        CHECK_NEAR(p.pos_after_bar[0], 0.6, 1e-9);            // the recalc close expired
        CHECK_NEAR(p.ledger_l35_after_bar[0], 1.0, 1e-9);     // 0.6 without the retired term
    }
    CHECK_NEAR(closed_qty_by_exit(p, "X", 1), 1.4, 1e-9);
    CHECK_NEAR(closed_qty_by_exit(p, "__close__B", 1), 0.0, 1e-9);   // the recalc close expired
    CHECK_NEAR(closed_qty_by_exit(p, "__close__B", 3), 1.0, 1e-9);   // 0.6 without the retired term
    CHECK(p.is_long());
    CHECK_NEAR(p.pos(), 0.6, 1e-9);                                  // 1.0 without the retired term
    CHECK(!p.ledger_has("B"));
    std::printf("   pos=%.6f ledger(B)=%.6f recalcs=%d\n", p.pos(), p.ledger("B"), p.recalc_calls);
    for (const Trade& t : p.all_trades())
        std::printf("   trade entry=%s exit=%s qty=%.6f exit_bar=%d\n",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.qty, t.exit_bar_index);
}

// E. A tokenized sole call capped SOLELY by this bar's other pending
//    callsites keeps the pre-F1 debit-by-target carry (unpinned against TV;
//    continuous with the no-cleanup zero-target A/A->B oracle).
static void test_same_bar_pending_cap_keeps_debit_by_target() {
    std::printf("-- E: a cap owed solely to same-bar pending sites keeps the carry --\n");
    constexpr uint64_t kSite1 = 8'101, kSite2 = 8'102, kSite3 = 8'103;
    Probe p(/*pooc=*/true);
    // bar0: lots D 0.02, E 0.03, A 0.10, B 0.10 -> position 0.25.
    // bar1: site1 close(D); close(E) collapse into the surviving close(E):
    //       fills 0.03 (FIFO: all of D, 0.01 of E), position 0.22, and
    //       leaves site1's standing 0.03 reservation on E (the prior-bar
    //       claim that keeps the position from going flat below).
    // bar2: close(B, qty=0.05) explicit -> position 0.17, ledger B 0.10.
    // bar3: site2 close(A): persistent_avail = 0.17 - 0.03 = 0.14 >= 0.10,
    //       target 0.10 (no cap). site3 close(B): persistent_avail 0.14 >=
    //       ledger 0.10 — the position / prior-bar claims would admit B
    //       whole — but site2's pending 0.10 leaves avail 0.04: target
    //       0.04, capped SOLELY by a same-bar site. Flush: A closes 0.10,
    //       B closes 0.04, position 0.03. The B ledger keeps its carry
    //       0.06 (an unconditional whole-retire would erase it).
    p.steps = {
        [](Probe& e) { e.entry("D", 0.02); e.entry("E", 0.03); e.entry("A", 0.10); e.entry("B", 0.10); },
        [](Probe& e) { e.close_at_site("D", kSite1); e.close_at_site("E", kSite1); },
        [](Probe& e) { e.close_qty("B", 0.05); },
        [](Probe& e) { e.close_at_site("A", kSite2); e.close_at_site("B", kSite3); },
        [](Probe& e) {
            e.ledger_l35_after_bar.push_back(e.ledger("B"));   // bar4, what bar3 left
            e.pos_after_bar.push_back(e.pos());
        },
        nullptr,
    };
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) bars.push_back(flat_bar(i, 100.0));
    p.run(bars.data(), (int)bars.size());
    CHECK_NEAR(closed_qty_by_exit(p, "__close__E", 1), 0.03, 1e-9);
    CHECK_NEAR(p.site_reserved(kSite1, "E"), 0.03, 1e-9);   // the prior-bar claim
    CHECK_NEAR(closed_qty_by_exit(p, "__close__B", 2), 0.05, 1e-9);
    CHECK_NEAR(closed_qty_by_exit(p, "__close__A", 3), 0.10, 1e-9);
    CHECK_NEAR(closed_qty_by_exit(p, "__close__B", 3), 0.04, 1e-9);   // the same-bar cap
    CHECK(p.ledger_l35_after_bar.size() == 1);
    if (!p.ledger_l35_after_bar.empty()) {
        CHECK_NEAR(p.pos_after_bar[0], 0.03, 1e-9);
        CHECK_NEAR(p.ledger_l35_after_bar[0], 0.06, 1e-9);   // the carry survives
    }
    CHECK(!p.ledger_has("A"));
    std::printf("   pos=%.6f ledger(B)=%.6f ledger(E)=%.6f\n",
                p.pos(), p.ledger("B"), p.ledger("E"));
    for (const Trade& t : p.all_trades())
        std::printf("   trade entry=%s exit=%s qty=%.6f exit_bar=%d\n",
                    t.entry_id.c_str(), t.exit_id.c_str(), t.qty, t.exit_bar_index);
}

int main() {
    test_pooc_capped_close_retires_ledger();
    test_pooc_ledger_absent_right_after_capped_fill();
    test_deliberate_partial_keeps_remainder();
    test_deferred_close_suppression_recredits_retired_remainder();
    test_coof_recalc_close_recredits_retired_remainder();
    test_same_bar_pending_cap_keeps_debit_by_target();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
