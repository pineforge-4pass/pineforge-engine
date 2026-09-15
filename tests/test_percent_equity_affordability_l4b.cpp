/*
 * test_percent_equity_affordability.cpp — round 6: default percent_of_equity
 * sizing ABOVE 100% takes the unified design-market-entry-affordability gate
 * (placement half in strategy_entry, fill half in apply_filled_order_to_state)
 * exactly like default CASH / FIXED sizing:
 *
 *   admit iff lot_floored(resulting_position_qty)
 *               * max(tick(close(S)), tick(fill)) * pv * fx * margin/100
 *             <= placement_equity + max(1e-9, |placement_equity| * 1e-12)
 *
 * checked at placement against mark-to-market equity and again at the fill
 * against the same snapshot; a declined reversal keeps its CLOSING leg only.
 *
 * Pins (`lab tv`, 2026-09-04, NYSE:F 15, 2025-04-01..07-01, capital 10,000,
 * commission 0, entry every 50th bar while flat, close 5 bars later —
 * scratchpad/r5/pins/out-pin-{pct-afford,cash-afford-m100,cash-afford-m50}):
 *   pin-pct-afford       percent_of_equity 200, margin 100 -> 0 entries.
 *   pin-cash-afford-m100 strategy.cash 20,000,  margin 100 -> 0 entries.
 *   pin-cash-afford-m50  strategy.cash 20,000,  margin 50  -> entries fill:
 *                        1,982 shares (20,000 / signal close 10.09, floored)
 *                        at the 10.08 open.
 * percent_of_equity 200 on 10,000 sizes the same 20,000 notional as cash
 * 20,000, so its margin-50 shape mirrors the cash tape (no separate TV export).
 *
 * At or below 100% nothing changes: those entries never receive an
 * affordability snapshot and keep the pinned KI-54 / gap-reject /
 * gross-admission family (test_frozen_flat_gap_reject,
 * test_default_flat_market_gross_admission, test_affordability_fx).
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "oracle_fixture_config_shim.hpp"
#include "l4b_pending_projection_shim.hpp"

using namespace pineforge;
using pineforge::source::PendingOrder;

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

static Bar flat_bar(int64_t ts, double p) { return mk_bar(ts, p, p, p, p); }

namespace {

// NYSE:F — pointvalue 1, mintick 0.01, whole shares.
// Script chars (indexed by bar_index_):
//   'L' default-sized LONG market entry "L"    'S' default SHORT "S"
//   'l' explicit LONG "L" qty = entry_qty_     'C' strategy.close("L")
//   '.' nothing
class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe(double capital, double pct, double margin, bool enable_mc) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = pct;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin;
        margin_short_ = margin;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        margin_call_enabled_ = enable_mc;
    }
    std::string script;
    double entry_qty_ = 1.0;
    // Placement-time observations of the LAST default-sized entry call:
    // did it survive placement (a PendingOrder exists), and did it carry an
    // affordability snapshot (the unified gate's scope discriminator)?
    int default_entries_placed = 0;
    int default_entries_pending_after_call = 0;
    int default_entries_with_snapshot = 0;

    void on_source_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'L': default_entry("L", true); break;
            case 'S': default_entry("S", false); break;
            case 'l': strategy_entry("L", true, kNaN, kNaN, entry_qty_); break;
            case 'C': strategy_close("L"); break;
            default: break;
        }
    }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    double position_size() const { return signed_position_size(); }
    int trades_with_entry_id(const std::string& id) const {
        int n = 0;
        for (const auto& t : trades_) if (t.entry_id == id) ++n;
        return n;
    }
    void set_process_orders_on_close(bool enabled) {
        process_orders_on_close_ = enabled;
    }

private:
    void default_entry(const std::string& id, bool is_long) {
        ++default_entries_placed;
        strategy_entry(id, is_long);
        for (const auto& o : pending_orders_) {
            if (o.id != id || o.type != OrderType::MARKET) continue;
            ++default_entries_pending_after_call;
            if (std::isfinite(o.affordability_placement_equity)) {
                ++default_entries_with_snapshot;
            }
        }
    }
};

// pin-pct-afford: 200% of 10,000 at margin 100 on F @10.09 sizes
// floor(20,000 / 10.09) = 1,982 shares = 19,998.38 > 10,000 -> every entry is
// declined AT PLACEMENT (no PendingOrder, no fill, no trade row). With margin
// calls enabled nothing changes: no position ever opens, so nothing cascades.
// Pre-fix the engine opened 1,982 shares on a 10,000 account (KI-54 skips
// pct > 100 and no other gate ran).
void test_f_200pct_margin100_declined() {
    std::printf("-- F 200%% at margin 100: 0 entries (pin-pct-afford) --\n");
    for (bool mc : {false, true}) {
        Probe eng(10000.0, 200.0, 100.0, mc);
        eng.script = "L....L....";
        std::vector<Bar> bars;
        for (int i = 0; i < 10; ++i) {
            bars.push_back(mk_bar(1000 * (i + 1), 10.09, 10.12, 10.05, 10.09));
        }
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
        CHECK(eng.trade_count() == 0);
        CHECK(eng.default_entries_placed == 2);
        CHECK(eng.default_entries_pending_after_call == 0);   // placement decline
        CHECK(eng.default_entries_with_snapshot == 0);
    }
}

// Mirror of pin-cash-afford-m50 with percent sizing: 200% at margin 50 costs
// 1,982 * 10.09 * 0.5 = 9,999.19 <= 10,000 at placement; the 10.08 open is a
// favorable gap (max(10.09, 10.08) = 10.09) so the fill half admits, and the
// 1,982 shares fill at 10.08 exactly as TV's cash tape does.
void test_f_200pct_margin50_fills() {
    std::printf("-- F 200%% at margin 50: fills 1,982 @10.08 (cash-m50 mirror) --\n");
    Probe eng(10000.0, 200.0, 50.0, false);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.09),                          // L placed
        mk_bar(2000, 10.08, 10.10, 10.06, 10.09),       // fills @10.08
        flat_bar(3000, 10.09),                          // close placed
        flat_bar(4000, 10.09),                          // close fills
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.default_entries_pending_after_call == 1);
    CHECK(eng.default_entries_with_snapshot == 1);       // in scope: pct > 100
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).entry_id == "L");
        CHECK_NEAR(eng.get_trade(0).qty, 1982.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).entry_price, 10.08, 1e-9);
    }
}

// Exactly 100% is byte-identical: no affordability snapshot is attached (the
// order stays on the KI-54 / gap-reject family) and the all-in entry fills as
// before — floor(10,000 / 10.09) = 991 shares at the 10.08 open.
void test_f_100pct_control_unchanged() {
    std::printf("-- F 100%% control: no snapshot, fills 991 --\n");
    Probe eng(10000.0, 100.0, 100.0, false);
    eng.script = "L...";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.09),
        mk_bar(2000, 10.08, 10.10, 10.06, 10.09),
        flat_bar(3000, 10.09),
        flat_bar(4000, 10.09),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.default_entries_pending_after_call == 1);
    CHECK(eng.default_entries_with_snapshot == 0);       // out of scope
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 991.0, 1e-9);
    CHECK(eng.trade_count() == 0);
}

// 150% at margin 50 is 75% of equity: pct > 100 is not a blanket decline,
// the rule is the notional. floor(15,000 / 10) = 1,500 shares * 10 * 0.5 =
// 7,500 <= 10,000 -> fills.
void test_f_150pct_margin50_fills() {
    std::printf("-- F 150%% at margin 50: 75%% of equity fills --\n");
    Probe eng(10000.0, 150.0, 50.0, false);
    eng.script = "L..";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.00), flat_bar(2000, 10.00), flat_bar(3000, 10.00),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 1500.0, 1e-9);
}

// The fill half: 200% at margin 50 on a 10.00 close sizes 2,000 shares
// (10,000 * 0.5 = 10,000, an exact tie -> admitted at placement). A 10.01
// open costs 10,010 > 10,000 -> NOT filled (no trade row, position flat);
// a 9.99 open is favorable and fills 2,000 shares. POOC fills at the tie.
void test_f_200pct_margin50_gap_up_declined_at_fill() {
    std::printf("-- F 200%% at margin 50: gap-up fill declined, gap-down fills --\n");
    {
        Probe eng(10000.0, 200.0, 50.0, false);
        eng.script = "L..";
        std::vector<Bar> bars = {
            flat_bar(1000, 10.00),
            mk_bar(2000, 10.01, 10.03, 9.99, 10.00),    // 2,000 * 10.01 * .5 > 10,000
            flat_bar(3000, 10.00),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.default_entries_pending_after_call == 1);   // placement admitted
        CHECK(eng.position_side_ == PositionSide::FLAT);
        CHECK(eng.trade_count() == 0);
    }
    {
        Probe eng(10000.0, 200.0, 50.0, false);
        eng.script = "L..";
        std::vector<Bar> bars = {
            flat_bar(1000, 10.00),
            mk_bar(2000, 9.99, 10.02, 9.98, 10.00),
            flat_bar(3000, 10.00),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 2000.0, 1e-9);
    }
    {
        Probe eng(10000.0, 200.0, 50.0, false);
        eng.set_process_orders_on_close(true);
        eng.script = "L..";
        std::vector<Bar> bars = {
            flat_bar(1000, 10.00),
            mk_bar(2000, 10.01, 10.03, 9.99, 10.00),
            flat_bar(3000, 10.00),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 2000.0, 1e-9);
    }
}

// Reversal at 200% / margin 100: a 500-share long (explicit, 5,000 <= 10,000)
// is held; the default short at close 10.20 sizes floor(2 * 10,100 / 10.20)
// = 1,980 shares = 20,196 > MTM 10,100 -> the ENTRY leg is declined at
// placement, the order survives close-only, and the CLOSING leg executes at
// the next open under the short's id. No short is opened.
void test_f_200pct_reversal_close_leg_only() {
    std::printf("-- F 200%% reversal: close leg executes, no new entry --\n");
    Probe eng(10000.0, 200.0, 100.0, false);
    eng.entry_qty_ = 500.0;
    eng.script = "l.S...";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.00),      // l placed
        flat_bar(2000, 10.00),      // 500 @10.00 fills
        flat_bar(3000, 10.20),      // S placed: 20,196 > 10,100 -> close-only
        flat_bar(4000, 10.20),      // close leg fills
        flat_bar(5000, 10.20),
        flat_bar(6000, 10.20),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.default_entries_pending_after_call == 1);   // close-only survives
    CHECK(eng.position_side_ == PositionSide::FLAT);      // pre-fix: SHORT 1,980
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).qty, 500.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).exit_price, 10.20, 1e-9);
    }
    CHECK(eng.trades_with_entry_id("S") == 0);
}

// Reversal declined at the FILL: 200% / margin 50, long 500 @10.00 held, the
// short at close 10.00 sizes 2,000 shares = 10,000 * 0.5 = 10,000 <= MTM
// 10,000 (tie, admitted at placement); the fill opens at 10.10 -> 10,100 >
// 10,000 -> entry leg dropped at the fill, closing leg executes at 10.10.
void test_f_200pct_reversal_declined_at_fill_closes_only() {
    std::printf("-- F 200%% reversal declined at fill: close leg only --\n");
    Probe eng(10000.0, 200.0, 50.0, false);
    eng.entry_qty_ = 500.0;
    eng.script = "l.S...";
    std::vector<Bar> bars = {
        flat_bar(1000, 10.00),
        flat_bar(2000, 10.00),
        flat_bar(3000, 10.00),                          // S placed (tie)
        mk_bar(4000, 10.10, 10.12, 10.08, 10.10),       // 2,000 * 10.10 * .5 > 10,000
        flat_bar(5000, 10.10),
        flat_bar(6000, 10.10),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).qty, 500.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).exit_price, 10.10, 1e-9);
    }
    CHECK(eng.trades_with_entry_id("S") == 0);
}

}  // namespace

int main() {
    std::printf("--- percent_equity_affordability ---\n");
    test_f_200pct_margin100_declined();
    test_f_200pct_margin50_fills();
    test_f_100pct_control_unchanged();
    test_f_150pct_margin50_fills();
    test_f_200pct_margin50_gap_up_declined_at_fill();
    test_f_200pct_reversal_close_leg_only();
    test_f_200pct_reversal_declined_at_fill_closes_only();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
