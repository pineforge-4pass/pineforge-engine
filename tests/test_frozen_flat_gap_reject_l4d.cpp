// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

/*
 * test_frozen_flat_gap_reject.cpp — TradingView's fill-time REJECTION of a
 * frozen 100%-of-equity true-flat MARKET entry whose gapped fill price pushes
 * the frozen-quantity notional past the sizing equity at all.
 *
 * Rule (design-cntvxiao-gap-reject, PANEL-CLEARED; widened to commissioned
 * entries by the round-7 market-entry-admission pin): a pending MARKET entry
 * created by high-level strategy.entry with omitted qty (frozen default sizing,
 * percent_of_equity == 100%), direction-appropriate margin == 100, placed
 * TRUE-FLAT (created flat, not a same-bar paired close/reentry) and still FLAT
 * at fill, is silently dropped (no trade row) at fill when:
 *
 *   |frozen_default_qty| * slipped_fill * pv * fx * margin/100
 *     >  sizing_equity
 *        + max(1e-9, |sizing_equity| * 1e-12)
 *
 * Direction-symmetric (long AND short). ANY positive shortfall rejects — there
 * is NO one-lot amnesty and the opening commission is NOT part of the test
 * (campaign notes log-20260905t071818z-e57e7235 / log-20260905t071819z-
 * ece9b623, lab tv tapes scratchpad/r7/pins/macd1d-mktadmit-*: 0.1%
 * commission, 206 placements, 0 violations; replayed on the registry bars by
 * test_market_admission_commission). pct<100 or gap-DOWN entries are
 * untouched, and a commissioned entry whose cost fits but whose cost + fee
 * does not keeps the KI-61 fill-then-trim path. See the gate in
 * engine_fills.cpp apply_filled_order_to_state for the evidence trail.
 *
 * RED-4  SHORT true-flat zero-comm above-lot gap  -> rejected (FLAT, no rows).
 * RED-6  rejected SHORT emits NO rows AT ALL, incl. the entry-bar margin-call
 *        trim rows the pre-fix engine produced.
 * RED-5  sub-lot positive-shortfall gap-up (qty_step>0) -> rejected (FLAT,
 *        no rows).
 * GREEN-B strategy.exit bracket bound to a flat-dropped entry id is inert
 *        (no phantom exit fill, no crash).
 * RED-7  commissioned twin of RED-6 (10% fee, 9 x 120 = 1080 > 1000): rejected
 *        (FLAT, no rows) — re-pinned by the round-7 tapes; it used to fill
 *        and take a 4-lot Margin-call trim.
 * GREEN-C commissioned fee-only shortfall (9 x 110 = 990 <= 1000 < 990 + 99):
 *        fills then takes the KI-61 one-lot Margin-call trim.
 * GREEN-D pct=99 twin: fills (rule requires EXACTLY 100).
 * GREEN-E gap-DOWN true-flat: fills with the frozen qty.
 */

#include <cmath>
#include <cstdio>
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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Scripted probe. All prices on-tick (mintick 0.01) so the zero-slippage
// directional snap is an identity and fills land exactly at the bar prices.
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double pct, double capital, double qty_step,
          double commission_pct, bool enable_mc) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = pct;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_pct;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = qty_step;
        process_orders_on_close_ = false;
        margin_call_enabled_ = enable_mc;
    }
    // 'L' = default long entry, 'S' = default short entry,
    // 'X' = default long entry + a protective strategy.exit bracket bound to
    //       it (stop below entry), '.' = nothing.
    std::string script;
    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': strategy_entry("L", true); break;
            case 'S': strategy_entry("S", false); break;
            case 'X':
                strategy_entry("L", true);
                strategy_exit("LX", "L", kNaN, /*stop_price=*/80.0);
                break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    double position_size() const { return signed_position_size(); }
    std::string exit_comment(int i) const {
        return closed_trade_exit_comment(i);
    }
    const std::vector<Trade>& all_trades() const { return trades_; }
};

// RED-4. SHORT true-flat, zero commission, gap ABOVE the signal close (open
// 102 > close 100). For a short that price is FAVORABLE, but the frozen-qty
// notional 100*102 = 10200 exceeds equity 10000 by far more than one lot
// (qty_step 0 -> only the float guard). margin_short_ == 100. The entry is
// silently dropped; margin calls are disabled so the pre-fix engine would
// simply HOLD the 100-lot short here.
void test_short_true_flat_above_lot_gap_rejected() {
    std::printf("-- RED-4: short true-flat above-lot gap rejected --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/10000.0, /*qty_step=*/0.0,
              /*commission_pct=*/0.0, /*enable_mc=*/false);
    eng.script = "S..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // S placed: frozen 100, eq 10000
        mk_bar(2000, 102, 103, 101, 102),   // gap up: 100*102 = 10200 -> DROP
        mk_bar(3000, 102, 102, 102, 102),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: SHORT
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// RED-6. The rejection must emit NO rows AT ALL — including the entry-bar
// "Margin call" trim rows the pre-fix engine produced. Margin calls ENABLED:
// pre-fix the short fills 10@120 and the finite-price cascade trims it on the
// entry bar (high 125), emitting a Margin-call row. Post-fix the entry is
// dropped before the fill, so process_margin_call sees FLAT and does nothing.
void test_rejected_short_emits_no_margin_call_rows() {
    std::printf("-- RED-6: rejected short emits no rows incl. margin trim --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/1000.0, /*qty_step=*/1.0,
              /*commission_pct=*/0.0, /*enable_mc=*/true);
    eng.script = "S.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // S placed: frozen 10, eq 1000
        mk_bar(2000, 120, 125,  80, 110),   // gap up: 10*120 = 1200 > 1120 DROP
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);                     // pre-fix: 1 margin call
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
}

// RED-5. Sub-lot positive shortfall REJECTS. qty_step 1: frozen 100 @ close
// 100; fill 100.5 -> notional 10050, a shortfall of 50 over equity 10000 —
// positive but under one lot (qty_step*fill = 100.5). TV re-checks the frozen
// margin against the sizing-equity snapshot at fill and cancels on ANY
// positive shortfall — no one-lot amnesty. Evidence: ycelestine77 33/33
// true-flat sub-lot-shortfall rejects on open-uptick fill bars (+0.01..+0.32);
// cntvxiao census 0/556 TV positive-shortfall admissions.
void test_sub_lot_positive_shortfall_rejected() {
    std::printf("-- RED-5: sub-lot positive shortfall rejects --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/10000.0, /*qty_step=*/1.0,
              /*commission_pct=*/0.0, /*enable_mc=*/true);
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100,   100,   100,   100),     // frozen floor(100)=100
        mk_bar(2000, 100.5, 101,   100.5, 100.5),   // shortfall 50 > 0 -> DROP
        mk_bar(3000, 100.5, 100.5, 100.5, 100.5),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-fix: LONG
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// GREEN-B. Dangling-exit safety. A strategy.exit bracket ("LX", from_entry
// "L", protective stop 80) is armed on the same bar as the default long "L".
// The long is flat-dropped by the gap-reject rule, so its bracket is bound to
// an id that never opened. A later drop through 80 must NOT manufacture a
// phantom exit fill or crash: the exit legs are inert.
void test_dangling_exit_bracket_is_inert() {
    std::printf("-- GREEN-B: dangling exit bracket on a flat-dropped entry --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/10000.0, /*qty_step=*/0.0,
              /*commission_pct=*/0.0, /*enable_mc=*/false);
    eng.script = "X..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // L + LX(stop 80) armed
        mk_bar(2000, 102, 103, 101, 102),   // L gap-up -> DROPPED
        mk_bar(3000,  79,  79,  79,  79),    // through stop 80 -> nothing to hit
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// RED-7. Commissioned twin of RED-6. Signal sizing reserves the 10% fee:
// floor(1000/1.1/100) = 9. The 120 fill costs 9 x 120 = 1080 > 1000, and the
// round-7 tapes show the commission is not part of TV's test: the entry is
// REJECTED outright — no fill, no entry-bar trim. (Before the pin the engine
// filled 9 @ 120 and trimmed 4, the shape TV's z8830 probes never show.)
// Mirrors test_commissioned_frozen_all_in_true_flat_gap_is_rejected.
void test_commissioned_all_in_gap_rejected() {
    std::printf("-- RED-7: commissioned all-in gap over equity rejected --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/1000.0, /*qty_step=*/1.0,
              /*commission_pct=*/10.0, /*enable_mc=*/true);
    eng.script = "L.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // frozen floor(1000/1.1/100)=9
        mk_bar(2000, 120, 125,  80, 110),   // 9*120 = 1080 > 1000 -> DROP
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);                     // pre-pin: 1 margin call
    CHECK(eng.position_side_ == PositionSide::FLAT);   // pre-pin: LONG 5
    CHECK_NEAR(eng.position_size(), 0.0, 1e-9);
}

// GREEN-C. Commissioned fee-only shortfall: the same 9 lots at a 110 fill
// cost 990 <= 1000 (admitted, the fee excluded), but 990 + the 99 opening fee
// is unaffordable, so the KI-61 entry-bar affordability trim fires: restore
// 9 - (1000 - 99)/110 = 0.809 -> floors to 0 -> the opening event's one-lot
// fallback closes 1 @ 110, leaving 8. (Tape shape: NYSE:F 2025-07-29 896 @
// 11.29 vs equity 10125.50, trimmed on the entry bar.)
void test_commissioned_fee_only_shortfall_fills_then_trims() {
    std::printf("-- GREEN-C: commissioned fee-only shortfall fills then trims --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/1000.0, /*qty_step=*/1.0,
              /*commission_pct=*/10.0, /*enable_mc=*/true);
    eng.script = "L.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // frozen floor(1000/1.1/100)=9
        mk_bar(2000, 110, 115,  80, 105),   // 9*110 = 990 <= 1000 -> fills, trims 1
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK_NEAR(eng.all_trades()[0].qty, 1.0, 1e-9);
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 8.0, 1e-9);
}

// GREEN-D. pct=99 twin of RED-3's arithmetic — the flag is set ONLY at exactly
// 100%, so pct=99 is never gap-rejected: the entry FILLS. frozen
// floor(1000*0.99/100)=9; the 120 fill is over budget by restore 0.6667, which
// floors sub-lot (qty_step 1), so the broker closes one whole contract and the
// position holds 8. That floor-zero lot is the generic TV rule and carries no
// side or commission conditioning — this is the commission-free LONG shape.
void test_pct99_twin_fills() {
    std::printf("-- GREEN-D: pct=99 twin fills (rule requires exactly 100) --\n");
    Probe eng(/*pct=*/99.0, /*capital=*/1000.0, /*qty_step=*/1.0,
              /*commission_pct=*/0.0, /*enable_mc=*/true);
    eng.script = "L.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // frozen floor(9.9)=9
        mk_bar(2000, 120, 125,  80, 110),   // over budget, sub-lot restore
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // NOT gap-rejected
    CHECK_NEAR(eng.position_size(), 8.0, 1e-9);
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
}

// GREEN-E. Gap-DOWN true-flat all-in: notional 100*98 = 9800 < equity 10000,
// so the rule never fires — the entry fills with the frozen qty (the divisor
// is close(S), not the lower fill price).
void test_gap_down_true_flat_fills() {
    std::printf("-- GREEN-E: gap-down true-flat fills with frozen qty --\n");
    Probe eng(/*pct=*/100.0, /*capital=*/10000.0, /*qty_step=*/0.0,
              /*commission_pct=*/0.0, /*enable_mc=*/false);
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // frozen 100
        mk_bar(2000,  98,  98,  98,  98),   // gap down: 100*98 = 9800 <= 10000
        mk_bar(3000,  98,  98,  98,  98),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 100.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

}  // namespace

int main() {
    std::printf("--- frozen_flat_gap_reject ---\n");
    test_short_true_flat_above_lot_gap_rejected();
    test_rejected_short_emits_no_margin_call_rows();
    test_sub_lot_positive_shortfall_rejected();
    test_dangling_exit_bracket_is_inert();
    test_commissioned_all_in_gap_rejected();
    test_commissioned_fee_only_shortfall_fills_then_trims();
    test_pct99_twin_fills();
    test_gap_down_true_flat_fills();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
