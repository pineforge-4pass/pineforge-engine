/*
 * test_explicit_qty_fill_admission.cpp — TradingView's fill-time DECLINE of an
 * EXPLICIT-qty (caller passed a finite qty) true-flat MARKET entry whose
 * next-bar fill gaps adversely enough that the position notional at the SLIPPED
 * FILL price overshoots the placement-time equity snapshot.
 *
 * Rule (design-explicit-qty-fill-admission, GO — probe-68 pinned): a pending
 * MARKET entry from strategy.entry with a FINITE explicit qty, created TRUE-FLAT
 * (created_position_side==FLAT && !created_after_position_close_in_bar) and still
 * FLAT at fill, is silently DROPPED (no trade row) at fill when
 *
 *   |qty| * slipped_fill * pv * fx * (margin_pct/100)
 *     >  max(placement_equity, |qty| * slipped_signal_close * pv * fx * margin/100)
 *        + max(1e-9, |placement_equity| * 1e-12)
 *
 * with margin_pct > 0 and ZERO structural slack (float guard only — probe-68
 * kills the one-lot term). The slipped-signal-close notional floors the
 * threshold so a fill AT/BELOW the slipped signal close (POOC, or a no-gap /
 * favorable open) is a structural no-op even with slippage != 0; only an ADVERSE
 * gap beyond the slip can decline. Commission is EXCLUDED from the predicate.
 *
 * This is the EXPLICIT-QTY sibling of the shipped frozen-omitted-qty gap-reject
 * (test_frozen_flat_gap_reject.cpp); that fix deliberately left this path alone.
 * The signal-time gate in strategy_entry (~:139-158) stays the first line of
 * defense; this is the fill-time re-check.
 *
 * Evidence anchors: data/probes/pf-probe-allin-floor-comm0 (4,740 from-flat
 * attempts, decline iff fill notional > equity, zero slack, 99.94%);
 * mdfe3757-trade-strategy-v8-4-pine-v6-ready (306/306 separation).
 *
 * RED-1  flat explicit all-in, comm 0, fill +1 mintick above signal -> DECLINED.
 * RED-2  same WITH commission > 0 -> STILL DECLINED (commission not in predicate).
 * GREEN-A favorable slip (fill below close), zero-headroom  -> ADMITS full qty.
 * GREEN-B exact tie (fill == close, no slippage)            -> ADMITS.
 * GREEN-C headroom (qty at 50% equity) + big adverse gap    -> ADMITS.
 * GREEN-D commissioned all-in favorable-slip                -> ADMITS (fill happens).
 * GREEN-E margin>100 characterization + margin==0 inertness.
 * GREEN-F priced (stop=) entry adverse gap + RAW strategy.order -> unaffected.
 * GREEN-G POOC=true with slippage>0                         -> no spurious decline.
 * GREEN-H same-bar close-then-explicit-reentry (after-close) -> NOT declined.
 * GREEN-I strategy.exit bracket bound to a declined entry   -> inert, no crash.
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

// Scripted probe. All prices on-tick (mintick 0.01) so the directional
// mintick snap in apply_slippage is an identity and fills land exactly at the
// bar prices. Script chars (indexed by bar_index_):
//   'L' explicit LONG  market entry, qty = entry_qty_
//   'S' explicit SHORT market entry, qty = entry_qty_
//   'P' explicit LONG  STOP entry (priced), stop = stop_, qty = entry_qty_
//   'R' RAW  LONG  order (strategy.order), qty = entry_qty_
//   'B' explicit LONG market entry + protective strategy.exit stop = exit_stop_
//   'H' immediate close of "E" + explicit LONG reentry "R2" qty = reentry_qty_
//   '.' nothing
class Probe : public BacktestEngine {
public:
    Probe(double capital, double comm_pct, int slippage, double margin,
          bool pooc, bool enable_mc) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::FIXED;   // irrelevant: entries pass explicit qty
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = comm_pct;
        margin_long_ = margin;
        margin_short_ = margin;
        slippage_ = slippage;
        syminfo_mintick_ = 0.01;
        qty_step_ = 0.0;                        // float guard only (zero slack)
        pyramiding_ = 10;                        // allow the H reentry as a fresh open
        process_orders_on_close_ = pooc;
        margin_call_enabled_ = enable_mc;
    }
    std::string script;
    double entry_qty_ = 100.0;
    double reentry_qty_ = 100.0;
    double stop_ = kNaN;
    double exit_stop_ = kNaN;

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("E", true,  kNaN, kNaN, entry_qty_); break;
            case 'S': strategy_entry("E", false, kNaN, kNaN, entry_qty_); break;
            case 'P': strategy_entry("E", true,  kNaN, stop_, entry_qty_); break;
            case 'R': strategy_order("E", true,  entry_qty_); break;
            case 'B':
                strategy_entry("E", true, kNaN, kNaN, entry_qty_);
                strategy_exit("EX", "E", kNaN, /*stop_price=*/exit_stop_);
                break;
            case 'H':
                strategy_close("E", "", kNaN, kNaN, /*immediately=*/true);
                strategy_entry("R2", true, kNaN, kNaN, reentry_qty_);
                break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    double position_size() const { return signed_position_size(); }
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    const std::vector<Trade>& all_trades() const { return trades_; }
};

// RED-1. Flat explicit all-in (qty = equity/close = 100), zero commission, no
// slippage. The fill gaps +1 mintick ABOVE the signal close (open 100.01):
// notional 100*100.01 = 10001 > equity 10000 -> DECLINED (flat, 0 trades).
// Pre-fix: fills 100@100.01 and the engine holds LONG 100.
void test_red1_flat_all_in_adverse_gap_declined() {
    std::printf("-- RED-1: flat all-in +1mintick adverse gap declined --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,    100,    100,    100),      // E placed, eq 10000
        mk_bar(2000, 100.01, 100.02, 100.00, 100.01),   // 100*100.01=10001 -> DROP
        mk_bar(3000, 100.01, 100.01, 100.01, 100.01),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// RED-2. Same shape WITH commission 0.1%. Commission is EXCLUDED from the
// predicate; the overage here is NOTIONAL (fill 100.01), so the entry is STILL
// DECLINED regardless of the fee. Pre-fix: fills.
void test_red2_commissioned_adverse_gap_still_declined() {
    std::printf("-- RED-2: commissioned adverse gap still declined --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.1, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,    100,    100,    100),
        mk_bar(2000, 100.01, 100.02, 100.00, 100.01),   // notional 10001 -> DROP
        mk_bar(3000, 100.01, 100.01, 100.01, 100.01),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// GREEN-A. Favorable slip: fill BELOW the signal close (open 99.99) at
// zero-headroom all-in (qty 100). Notional 100*99.99 = 9999 <= equity 10000
// -> ADMITS with the full qty.
void test_greenA_favorable_slip_admits() {
    std::printf("-- GREEN-A: favorable-slip zero-headroom admits --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,   100,   100,   100),
        mk_bar(2000, 99.99, 100.0, 99.98, 99.99),       // 100*99.99=9999 <= 10000
        mk_bar(3000, 99.99, 99.99, 99.99, 99.99),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
}

// GREEN-B. Exact tie: fill == signal close (open 100, no slippage). Notional
// 100*100 = 10000 is NOT strictly greater than equity 10000 (+ float guard)
// -> ADMITS.
void test_greenB_exact_tie_admits() {
    std::printf("-- GREEN-B: exact-tie admits --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),               // 100*100 = 10000 == eq
        mk_bar(3000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
}

// GREEN-C. Headroom: qty sized at 50% of equity (50 lots = 5000). Even a big
// adverse gap (+50%, open 150) leaves notional 50*150 = 7500 <= equity 10000
// -> ADMITS. probe-05/06 shape: equity dominates the threshold.
void test_greenC_headroom_big_gap_admits() {
    std::printf("-- GREEN-C: headroom + big adverse gap admits --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 50.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 150, 151, 149, 150),               // 50*150 = 7500 <= 10000
        mk_bar(3000, 150, 150, 150, 150),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 50.0, 1e-9);
}

// GREEN-D. Commissioned all-in favorable-slip: fill below close (open 99.99),
// commission 0.1% is NOT in the predicate so notional 9999 <= 10000 -> the
// admission itself passes and the position OPENS. Margin call disabled here so
// the KI-61-family entry-bar trim does not perturb the assertion under test
// (the trim machinery is exercised by test_margin_call / the frozen-gap tests).
void test_greenD_commissioned_favorable_admits() {
    std::printf("-- GREEN-D: commissioned all-in favorable-slip admits --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.1, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,   100,   100,   100),
        mk_bar(2000, 99.99, 100.0, 99.98, 99.99),       // notional 9999 <= 10000
        mk_bar(3000, 99.99, 99.99, 99.99, 99.99),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // the fill happened
    CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
}

// GREEN-E. margin>100 characterization + margin==0 inertness.
//   E1: margin_pct=200 (sub-1x leverage). The signal-time gate caps all-in at
//       qty = equity/(close*2) = 50. A favorable/no-gap fill (open 100) keeps
//       notional 50*100*2 = 10000 == threshold -> ADMITS (unchanged from
//       pre-fix; the gate applies the same margin/100 arithmetic on both sides).
//   E2: margin_pct=0 -> the candidate flag is never set (margin>0 required) AND
//       the signal-time gate is inert, so a wild adverse gap still ADMITS.
void test_greenE_margin_variants() {
    std::printf("-- GREEN-E1: margin=200 favorable/tie admits (char.) --\n");
    {
        Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/200.0,
                  /*pooc=*/false, /*enable_mc=*/false);
        eng.entry_qty_ = 50.0;
        eng.script = "L..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100, 100, 100, 100),
            mk_bar(2000, 100, 100, 100, 100),           // 50*100*2 = 10000 == thr
            mk_bar(3000, 100, 100, 100, 100),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 50.0, 1e-9);
    }
    std::printf("-- GREEN-E2: margin=0 gate inert (admits wild gap) --\n");
    {
        Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/0.0,
                  /*pooc=*/false, /*enable_mc=*/false);
        eng.entry_qty_ = 100.0;
        eng.script = "L..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100, 100, 100, 100),
            mk_bar(2000, 200, 201, 199, 200),           // huge gap, margin 0 -> admit
            mk_bar(3000, 200, 200, 200, 200),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
    }
}

// GREEN-F. Priced (stop=) entry + RAW strategy.order are UNAFFECTED.
//   F1: an explicit-qty LONG STOP entry (stop 100) that arms and fills on an
//       adverse gap keeps its own price; it carries no admission snapshot
//       (type == ENTRY, not MARKET) so the fill gate never fires -> fills.
//   F2: a RAW strategy.order all-in adverse gap never sets the candidate flag
//       -> fills.
void test_greenF_priced_and_raw_unaffected() {
    std::printf("-- GREEN-F1: priced (stop) entry adverse gap unaffected --\n");
    {
        Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
                  /*pooc=*/false, /*enable_mc=*/false);
        eng.entry_qty_ = 100.0;
        eng.stop_ = 100.0;                              // stop trigger at 100
        eng.script = "P..";
        std::vector<Bar> bars = {
            mk_bar(1000, 90, 90, 90, 90),               // P armed (stop 100)
            mk_bar(2000, 101, 102, 100, 101),           // gaps through 100 -> fills
            mk_bar(3000, 101, 101, 101, 101),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
    }
    std::printf("-- GREEN-F2: RAW strategy.order adverse gap unaffected --\n");
    {
        Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
                  /*pooc=*/false, /*enable_mc=*/false);
        eng.entry_qty_ = 100.0;
        eng.script = "R..";
        std::vector<Bar> bars = {
            mk_bar(1000, 100,    100,    100,    100),
            mk_bar(2000, 100.01, 100.02, 100.00, 100.01),  // adverse: RAW unaffected
            mk_bar(3000, 100.01, 100.01, 100.01, 100.01),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
    }
}

// GREEN-G. POOC=true with slippage>0 must be a structural no-op. The order
// fills at the SAME bar close (100) with slippage 1 tick -> slipped_fill
// 100.01, notional 10001. The slipped-signal-close notional (also 100.01 ->
// 10001) floors the threshold, so the fill is admitted (a fill at the slipped
// signal close is never a decline). Without the floor the pure "notional >
// equity 10000" rule would spuriously drop this.
void test_greenG_pooc_slippage_no_op() {
    std::printf("-- GREEN-G: POOC + slippage>0 no spurious decline --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/1, /*margin=*/100.0,
              /*pooc=*/true, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // E placed AND filled at close (POOC)
        mk_bar(2000, 100, 100, 100, 100),
        mk_bar(3000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
}

// GREEN-H. Same-bar close-then-explicit-reentry. Bar0 opens a small long (qty
// 1); bar1 immediately closes it AND places an all-in explicit reentry "R2"
// from the now-flat position (created_after_position_close_in_bar == true).
// Bar2 gaps +1 mintick adverse. Because the reentry is NOT created true-flat
// (the after-close flag is set), it is EXCLUDED from the fill gate and fills
// despite the adverse gap. A true-flat all-in here would be declined (RED-1).
void test_greenH_close_reentry_not_declined() {
    std::printf("-- GREEN-H: same-bar close-then-reentry not declined --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 1.0;          // bar0 'L' small long
    eng.reentry_qty_ = 100.0;      // bar1 'H' all-in reentry
    eng.script = "LH.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,    100,    100,    100),      // L placed (qty 1)
        mk_bar(2000, 100,    100,    100,    100),       // L fills @100; then close+reentry
        mk_bar(3000, 100.01, 100.02, 100.00, 100.01),   // R2 fills adverse -> NOT declined
        mk_bar(4000, 100.01, 100.01, 100.01, 100.01),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);       // reentry filled
    CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
}

// GREEN-I. Dangling-exit safety. A strategy.exit bracket ("EX", from_entry
// "E", protective stop 80) is armed on the same bar as the all-in explicit
// long "E". The entry is fill-declined by the adverse gap, so its bracket is
// bound to an id that never opened. A later drop through 80 must NOT
// manufacture a phantom exit fill or crash: the exit legs are inert.
void test_greenI_dangling_exit_inert() {
    std::printf("-- GREEN-I: exit bracket on a declined entry is inert --\n");
    Probe eng(/*capital=*/10000.0, /*comm=*/0.0, /*slip=*/0, /*margin=*/100.0,
              /*pooc=*/false, /*enable_mc=*/false);
    eng.entry_qty_ = 100.0;
    eng.exit_stop_ = 80.0;
    eng.script = "B..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,    100,    100,    100),      // E + EX(stop 80) armed
        mk_bar(2000, 100.01, 100.02, 100.00, 100.01),   // E adverse gap -> DECLINED
        mk_bar(3000,  79,     79,     79,     79),        // through stop 80 -> nothing
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG then exit
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

}  // namespace

int main() {
    std::printf("--- explicit_qty_fill_admission ---\n");
    test_red1_flat_all_in_adverse_gap_declined();
    test_red2_commissioned_adverse_gap_still_declined();
    test_greenA_favorable_slip_admits();
    test_greenB_exact_tie_admits();
    test_greenC_headroom_big_gap_admits();
    test_greenD_commissioned_favorable_admits();
    test_greenE_margin_variants();
    test_greenF_priced_and_raw_unaffected();
    test_greenG_pooc_slippage_no_op();
    test_greenH_close_reentry_not_declined();
    test_greenI_dangling_exit_inert();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
