/*
 * test_margin_admission_gate.cpp — KI-54: TradingView's fill-time margin
 * admission for FROZEN default-sized market orders.
 *
 *   same_dir    = position open AND order direction matches it
 *   reversal    = position open AND order direction opposes it
 *   free_funds  = same_dir ? sizing_equity - held_margin : sizing_equity
 *   admit_price = reversal ? fill_price : sizing_price
 *   required    = |qty| * admit_price * pointvalue * fx * margin_pct/100
 *   drop iff required > free_funds + eps       (silent: no trade row)
 *   scope: percent_of_equity default sizing with pct <= 100 ONLY
 *
 * Pins (see the gate comment in engine_fills.cpp for the evidence trail):
 *   A. Flat all-in (pct=100) zero-commission open on a gap-UP bar is REJECTED
 *      when the frozen-qty notional at the fill exceeds the sizing equity by
 *      more than one lot (design-cntvxiao-gap-reject). Flat opens still price
 *      the KI-54 add/reversal gate at the SIZING notional, but this narrower
 *      true-flat zero-comm all-in carve-out re-checks the FILL notional and
 *      silently drops the entry.
 *   B. Same-direction add at pct=100 is DECLINED — the held position keeps
 *      its capital committed, free_funds ~= 0. (pyramiding=2, so the
 *      decline comes from the margin gate, not the pyramiding limit —
 *      pin C proves the same setup fills when funded.)
 *   C. Same-direction add at pct=10 with one prior lot is ADMITTED —
 *      held ~= 0.1*equity, free_funds ~= 0.9*equity >> required.
 *   D. A TRUE REVERSAL prices at the FILL: an adverse gap on the fill bar
 *      pushes required past sizing_equity and the flip is DECLINED — the
 *      old position stays. (TV ground truth: the all-in flip specimen has
 *      0/1068 gap-up flip fills against a 50/50 gapping feed.)
 *   E. A reversal whose fill price EQUALS the sizing price is an exact
 *      required == free_funds tie at all-in — the epsilon must ADMIT it.
 *   F. CASH default sizing is exempt: a flat open with cash_value >
 *      equity (no floor invariant exists for CASH) must be ADMITTED —
 *      regression pin for the gate's percent_of_equity/pct<=100 scope.
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

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Scripted probe: per-bar action, all prices on-tick (mintick 0.01) so the
// zero-slippage directional snap is an identity.
class Probe : public BacktestEngine {
public:
    Probe(QtyType qty_type, double qty_value, int pyramiding) {
        initial_capital_ = 10000.0;
        default_qty_type_ = qty_type;
        default_qty_value_ = qty_value;
        commission_value_ = 0.0;
        pyramiding_ = pyramiding;
        // All-in probes hold fully-leveraged positions; forced liquidation is
        // not the mechanism under test.
        margin_call_enabled_ = false;
    }
    // 'L' = default long "L", 'A' = default long add "L2",
    // 'S' = default short "S", 'B' = default long then short in one
    // execution, 'C' = default short then long in one execution,
    // 'E' = explicit-qty long then short in one execution, '.' = nothing.
    std::string script;
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("L", true); break;
            case 'A': strategy_entry("L2", true); break;
            case 'S': strategy_entry("S", false); break;
            case 'B':
                strategy_entry("L", true);
                strategy_entry("S", false);
                break;
            case 'C':
                strategy_entry("S", false);
                strategy_entry("L", true);
                break;
            case 'E':
                strategy_entry("L", true, kNaN, kNaN, 1.0);
                strategy_entry("S", false, kNaN, kNaN, 1.0);
                break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    using BacktestEngine::qty_step_;
    using BacktestEngine::slippage_;
    using BacktestEngine::initial_capital_;
    using BacktestEngine::margin_long_;
    using BacktestEngine::margin_short_;
    const std::vector<Trade>& all_trades() const { return trades_; }
};

static void run_constant_100_script(Probe& eng, const std::string& script) {
    eng.script = script;
    std::vector<Bar> bars(5, mk_bar(1000, 100, 100, 100, 100));
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        bars[i].timestamp = (i + 1) * 1000;
    }
    eng.run(bars.data(), static_cast<int>(bars.size()));
}

// A. Flat all-in (pct=100) zero-comm open, gap UP: REJECTED. Frozen qty
//    10000/100 = 100; the fill notional 100*102 = 10200 exceeds the 10000
//    sizing equity by $200, far past the one-lot slack (qty_step 0 -> only the
//    float guard). The entry is silently dropped and the account stays flat.
//    (Pre-gap-reject this admitted and opened LONG 100 on the frozen notional.)
void test_flat_gap_up_rejected() {
    std::printf("-- A: flat all-in zero-comm gap-up rejected --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 102, 103, 101, 102),   // gap up: 100*102 = 10200 > 10000
        mk_bar(3000, 102, 102, 102, 102),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // was LONG 100
    CHECK(eng.trade_count() == 0);                      // no trade row
}

// B. Same-direction add at pct=100: DECLINED (free_funds ~= 0).
void test_all_in_same_dir_add_declined() {
    std::printf("-- B: all-in same-direction add declined --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 2);
    eng.script = "LA..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // L placed (frozen 100)
        mk_bar(2000, 100, 100, 100, 100),   // L fills: LONG 100 @100; L2 placed
        mk_bar(3000, 100, 100, 100, 100),   // L2: held=10000, free=0 -> DROP
        mk_bar(4000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);   // NOT 200
    CHECK(eng.trade_count() == 0);                 // no phantom fills/closes
}

// C. Same-direction add at pct=10 with one prior lot: ADMITTED.
//    held = 1000, free_funds = 9000, required = 1000.
void test_fractional_same_dir_add_admitted() {
    std::printf("-- C: fractional same-direction add admitted --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 10.0, 2);
    eng.script = "LA..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),   // L fills: LONG 10 @100; L2 placed
        mk_bar(3000, 100, 100, 100, 100),   // L2 fills: LONG 20
        mk_bar(4000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 20.0, 1e-9);
}

// D. TRUE reversal on an adverse-gap fill bar: DECLINED. Frozen short:
//    eq_S = 10000 + (110-100)*100 = 11000, qty = 11000/110 = 100,
//    sizing_price 110. Fill gaps to 111: required = 100*111 = 11100 >
//    11000 -> the flip is silently dropped; the LONG stays open and no
//    trade row is emitted.
void test_reversal_declined_on_adverse_gap() {
    std::printf("-- D: reversal declined on adverse-gap fill bar --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
    eng.script = "LS..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // L placed (frozen 100)
        mk_bar(2000, 100, 112, 99, 110),    // L fills @100; S placed
        mk_bar(3000, 111, 112, 110, 111),   // gap up: 100*111 > 11000 -> DROP
        mk_bar(4000, 111, 111, 111, 111),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // flip did NOT happen
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// E. Reversal at the exact tie (fill_price == sizing_price): required ==
//    free_funds at all-in; the epsilon must ADMIT it and the flip happens
//    with the frozen qty.
void test_reversal_admitted_at_exact_tie() {
    std::printf("-- E: reversal admitted at exact required==free tie --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
    eng.script = "LS..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 112, 99, 110),    // L fills @100; S placed (frozen
                                            //   100 = 11000/110, sizing 110)
        mk_bar(3000, 110, 111, 109, 110),   // fill 110 == sizing_price: tie
        mk_bar(4000, 110, 110, 110, 110),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        const Trade& t0 = eng.all_trades()[0];
        CHECK(t0.is_long);
        CHECK_NEAR(t0.exit_price, 110.0, 1e-9);
        CHECK_NEAR(t0.pnl, 1000.0, 1e-9);
    }
}

// F. CASH default sizing is exempt from the re-check: cash 20000 on 10000
//    capital sizes 200 lots @100 — no floor invariant bounds it by equity,
//    and no TV ground truth pins a decline, so the flat open must FILL.
//    (Pre-fix the gate declined every such entry: 445/445 on a real
//    transpiled cash probe, 73 trades -> 0.)
void test_cash_flat_open_admitted() {
    std::printf("-- F: cash default sizing exempt, flat open admitted --\n");
    Probe eng(QtyType::CASH, 20000.0, 1);
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // frozen 20000/100 = 200
        mk_bar(2000, 100, 100, 100, 100),   // must fill: LONG 200
        mk_bar(3000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 200.0, 1e-9);
}

}  // namespace

// G. Slippage > 0, SHORT reversal on a bar that does not gap at all.
//    The sell's real fill is close(S) - slip*mintick, which is exactly the
//    sizing price the qty was frozen against, so this is the same tie as E.
//    Comparing the RAW fill price against a slipped budget would decline it —
//    and would decline every short reversal on the most common bar shape
//    there is.
void test_slipped_short_reversal_zero_gap_admitted() {
    std::printf("-- G: slipped short reversal, zero gap, admitted --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
    eng.slippage_ = 2;                       // 2 ticks @ mintick 0.01
    eng.script = "LS..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),    // L placed
        mk_bar(2000, 100, 100, 100, 100),    // L fills (buy @100.02); S placed
        mk_bar(3000, 100, 100, 100, 100),    // no gap: sell fills @99.98
        mk_bar(4000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::SHORT);   // the flip happened
}

// H. Lot-step slack: with a real qty_step the frozen qty leaves an unspent
//    remainder in [0, qty_step*price). A reversal whose adverse gap costs
//    LESS than one lot of notional must be ADMITTED — the decline would be
//    decided by where the floor landed, not by affordability. A gap that
//    costs far more than one lot is still DECLINED.
void test_reversal_lot_step_slack() {
    std::printf("-- H: reversal within one lot of notional admitted --\n");
    {
        Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
        eng.qty_step_ = 0.01;                 // one lot @ ~100 = ~$1.00 notional
        eng.script = "LS..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100, 100, 100, 100),
            mk_bar(2000, 100, 100, 100, 100),   // LONG 100 @100
            mk_bar(3000, 100.01, 100.01, 100.01, 100.01),  // 1-tick gap: costs $1.00 < one lot
            mk_bar(4000, 100.01, 100.01, 100.01, 100.01),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::SHORT);   // admitted
    }
    {
        Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
        eng.qty_step_ = 0.01;
        eng.script = "LS..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100, 100, 100, 100),
            mk_bar(2000, 100, 100, 100, 100),   // LONG 100 @100
            mk_bar(3000, 101, 101, 101, 101),   // $100 over budget >> one lot
            mk_bar(4000, 101, 101, 101, 101),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);    // declined
    }
}

// I. A FRACTIONAL same-direction add is gated against MARK-TO-MARKET free
//    margin — pinned by data/probes/margin-basis-frac (pct=50, pyramiding=2):
//    TV admitted 1535/1538 adds while UNDERWATER and declined the in-profit
//    ones. At pct=50 the underwater add is admitted (free margin = cash =
//    0.5*equity >= required) and the profitable add is DECLINED (the position
//    marked up shrinks free margin below required). A cost-basis rule would
//    invert both, so this pin also refutes cost basis.
void test_fractional_add_marked_to_market() {
    std::printf("-- I: fractional add gated mark-to-market (TV-pinned) --\n");
    {   // UNDERWATER: price 100 -> 90. equity 9500, held 50*90=4500,
        // free 5000, required 52.78*90=4750 -> ADMITTED.
        std::printf("   I.1 underwater add admitted\n");
        Probe eng(QtyType::PERCENT_OF_EQUITY, 50.0, 2);
        eng.script = "LA..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100, 100, 100, 100),
            mk_bar(2000, 100, 110, 90, 90),   // L fills @100; down; L2 placed
            mk_bar(3000, 90, 90, 90, 90),
            mk_bar(4000, 90, 90, 90, 90),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK(eng.position_qty_ > 50.0);      // add went on
    }
    {   // PROFITABLE: price 100 -> 110. equity 10500, held 50*110=5500,
        // free 5000, required 47.73*110=5250 -> DECLINED (this is what TV does;
        // a cost-basis rule would admit it).
        std::printf("   I.2 profitable add declined\n");
        Probe eng(QtyType::PERCENT_OF_EQUITY, 50.0, 2);
        eng.script = "LA..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100, 100, 100, 100),
            mk_bar(2000, 100, 110, 90, 110),  // L fills @100; up; L2 placed
            mk_bar(3000, 110, 110, 110, 110),
            mk_bar(4000, 110, 110, 110, 110),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_qty_, 50.0, 1e-9);   // add dropped
    }
}

// J. Bankrupt account: sizing_equity <= 0 makes the frozen qty NEGATIVE, and
//    apply_qty_step returns it unfloored. The LEGACY path opened a negative-qty
//    position; every close of it then emitted a negative-qty trade row that
//    flipped the exported PnL sign (the KI-72 emission/accounting split).
//
//    KI-72 FIX: a default-sized percent_of_equity MARKET/RAW order whose frozen
//    sizing is NON-POSITIVE is now DECLINED CLEANLY (no fill, no trade row) —
//    a bankrupt account can afford nothing, symmetric on both sides. So the
//    order does not open and position_qty_ stays 0 (was -50 under the legacy
//    path). This is the exact behaviour test_short_reversal_emission pins from
//    the fill side; here it is pinned at the gate.
void test_negative_equity_reversal_declined_clean() {
    std::printf("-- J: bankrupt-account order declined cleanly (KI-72) --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
    eng.initial_capital_ = -5000.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),
        mk_bar(3000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK_NEAR(eng.position_qty_, 0.0, 1e-9);      // clean decline, no neg-qty open
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 0);                 // no corrupt trade row emitted
}

// K. margin > 100 (sub-1x leverage) breaks the flat-open invariant:
//    required = equity*pct/100*margin/100 > equity. The gate must not run.
void test_margin_above_100_flat_open_admitted() {
    std::printf("-- K: flat open at margin > 100 admitted --\n");
    Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
    eng.margin_long_ = 200.0;            // 0.5x leverage
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),
        mk_bar(3000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // admitted, not dropped
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);
}

// L. A MARKET entry that was same-direction when created can become a
// reversal when an earlier sibling flips the position at the shared next
// tick. TV rechecks this newly augmented transaction against free margin:
//
//   held margin       = live_qty(1) * price(100) = 100
//   reversal order    = close_qty(1) + new_qty(1) = 2 * 100 = 200
//   total requirement = 300
//
// With equity 299 the second order is silently declined, leaving the first
// reversal's LONG open. At the exact equity=300 boundary, required margin and
// held+transaction capital are equal and the second order is admitted; 301 is
// the funded control. This is pinned by the gb2wgkrtxs export: among common
// two-order timestamps, TV keeps both in 992/992 cases above held+transaction
// margin and only one in 470/471 cases below it.
void test_same_side_market_becomes_reversal_free_margin_gate() {
    std::printf("-- L: same-side market becomes reversal, free-margin gate --\n");
    {
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 299.0;
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 1);              // Seed closed by L only
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_qty_, 1.0, 1e-9);
    }
    {
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 300.0;
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 2);              // exact tie is admitted
        CHECK(eng.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(eng.position_qty_, 1.0, 1e-9);
    }
    {
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 301.0;
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(eng.position_qty_, 1.0, 1e-9);
    }
}

// M. Mutation-killing scope controls for the bounded GB2 gate. Every fixture
// starts one dollar below the 300-dollar fixed/default admission boundary, so
// accidentally widening exactly one guard turns the expected fill into a
// decline:
//   - explicit qty remains owned by strategy_entry's signal-time admission;
//   - both the held side and requested side must independently be 100% margin;
//   - an ordinary same-direction FIXED add never became a reversal;
//   - PERCENT_OF_EQUITY=100 remains owned by KI-54's frozen-sizing gate.
void test_same_side_role_change_scope_controls() {
    std::printf("-- M: same-side role-change scope controls --\n");
    {
        std::printf("   M.1 explicit qty is inert\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 299.0;
        run_constant_100_script(eng, "S.E..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(eng.position_qty_, 1.0, 1e-9);
    }
    {
        std::printf("   M.2 held-side margin != 100 is inert\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 299.0;
        eng.margin_long_ = 50.0;       // live held side before the second fill
        eng.margin_short_ = 100.0;     // requested side
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(eng.position_qty_, 1.0, 1e-9);
    }
    {
        std::printf("   M.3 requested-side margin != 100 is inert\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 299.0;
        eng.margin_long_ = 100.0;      // live held side before the second fill
        eng.margin_short_ = 50.0;      // requested side
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(eng.position_qty_, 1.0, 1e-9);
    }
    {
        std::printf("   M.4 ordinary same-direction FIXED add is inert\n");
        Probe eng(QtyType::FIXED, 1.0, 2);
        eng.initial_capital_ = 299.0;
        run_constant_100_script(eng, "LA...");
        CHECK(eng.trade_count() == 0);
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_qty_, 2.0, 1e-9);
    }
    {
        std::printf("   M.5 percent-of-equity role change stays in KI-54\n");
        Probe eng(QtyType::PERCENT_OF_EQUITY, 100.0, 1);
        eng.initial_capital_ = 300.0;
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        CHECK_NEAR(eng.position_qty_, 3.0, 1e-9);
    }
}

// N. Slippage-basis boundary. With 100 ticks at mintick .01, each buy books
// at 101 and each sell at 99 while the broker's matched mark remains raw 100.
// The role-changing order must therefore use raw 100 for open equity and held
// margin, but the requested transaction must use its slipped execution price.
//
// SHORT request after a SHORT->LONG first sibling:
//   realized=-2, open=-1 at raw 100, held=100, required=2*99=198
//   capital 301 => free=198 (admit); capital 300 => free=197 (decline).
// LONG request after a LONG->SHORT first sibling:
//   realized=-2, open=-1 at raw 100, held=100, required=2*101=202
//   capital 305 => free=202 (admit); capital 304 => free=201 (decline).
// These four edges kill raw-fill transaction pricing and slipped-mark
// substitutions independently in both directions.
void test_same_side_role_change_slippage_basis() {
    std::printf("-- N: same-side role-change slippage basis --\n");
    {
        std::printf("   N.1 slipped SHORT transaction exact tie admits\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 301.0;
        eng.slippage_ = 100;
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        if (eng.trade_count() == 2) {
            CHECK_NEAR(eng.all_trades()[0].entry_price, 99.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[0].exit_price, 101.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[1].entry_price, 101.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[1].exit_price, 99.0, 1e-9);
        }
    }
    {
        std::printf("   N.2 slipped SHORT transaction one dollar short declines\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 300.0;
        eng.slippage_ = 100;
        run_constant_100_script(eng, "S.B..");
        CHECK(eng.trade_count() == 1);
        CHECK(eng.position_side_ == PositionSide::LONG);
        if (eng.trade_count() == 1) {
            CHECK_NEAR(eng.all_trades()[0].entry_price, 99.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[0].exit_price, 101.0, 1e-9);
        }
    }
    {
        std::printf("   N.3 slipped LONG transaction exact tie admits\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 305.0;
        eng.slippage_ = 100;
        run_constant_100_script(eng, "L.C..");
        CHECK(eng.trade_count() == 2);
        CHECK(eng.position_side_ == PositionSide::LONG);
        if (eng.trade_count() == 2) {
            CHECK_NEAR(eng.all_trades()[0].entry_price, 101.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[0].exit_price, 99.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[1].entry_price, 99.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[1].exit_price, 101.0, 1e-9);
        }
    }
    {
        std::printf("   N.4 slipped LONG transaction one dollar short declines\n");
        Probe eng(QtyType::FIXED, 1.0, 1);
        eng.initial_capital_ = 304.0;
        eng.slippage_ = 100;
        run_constant_100_script(eng, "L.C..");
        CHECK(eng.trade_count() == 1);
        CHECK(eng.position_side_ == PositionSide::SHORT);
        if (eng.trade_count() == 1) {
            CHECK_NEAR(eng.all_trades()[0].entry_price, 101.0, 1e-9);
            CHECK_NEAR(eng.all_trades()[0].exit_price, 99.0, 1e-9);
        }
    }
}

int main() {
    std::printf("--- margin_admission_gate ---\n");
    test_flat_gap_up_rejected();
    test_all_in_same_dir_add_declined();
    test_fractional_same_dir_add_admitted();
    test_reversal_declined_on_adverse_gap();
    test_reversal_admitted_at_exact_tie();
    test_cash_flat_open_admitted();
    test_slipped_short_reversal_zero_gap_admitted();
    test_reversal_lot_step_slack();
    test_fractional_add_marked_to_market();
    test_negative_equity_reversal_declined_clean();
    test_margin_above_100_flat_open_admitted();
    test_same_side_market_becomes_reversal_free_margin_gate();
    test_same_side_role_change_scope_controls();
    test_same_side_role_change_slippage_basis();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
