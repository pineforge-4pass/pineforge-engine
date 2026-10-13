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
 * Pinned against TradingView behaviour (recorded tapes are kept outside this
 * repository). Whole-lot cent-tick fixtures use invented prices and timestamps:
 * 200% sizing on capital 10,000 requests notional 20,000. Margin 100 rejects
 * it; margin 50 admits it, including a favorable next-open gap.
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

// Synthetic instrument: pointvalue 1, mintick 0.01, whole lots.
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

// 200% of 10,000 at margin 100 sizes floor(20,000 / 125.50) = 159 lots.
// Cost = 159 * 125.50 = 19,954.50 > 10,000: each entry is declined AT
// PLACEMENT (no PendingOrder, no fill, no trade row), with margin calls both
// disabled and enabled. Pre-fix: KI-54 skips pct > 100 and no other gate ran.
void test_case_200pct_margin100_declined() {
    std::printf("-- synthetic 200%% at margin 100: 0 entries --\n");
    for (bool mc : {false, true}) {
        Probe eng(10000.0, 200.0, 100.0, mc);
        eng.script = "L....L....";
        std::vector<Bar> bars;
        for (int i = 0; i < 10; ++i) {
            bars.push_back(mk_bar(1000 * (i + 1), 125.50, 126.00, 124.50, 125.50));
        }
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::FLAT);
        CHECK(eng.trade_count() == 0);
        CHECK(eng.default_entries_placed == 2);
        CHECK(eng.default_entries_pending_after_call == 0);   // placement decline
        CHECK(eng.default_entries_with_snapshot == 0);
    }
}

// 200% at margin 50 costs 159 * 125.50 * 0.5 = 9,977.25 <= 10,000.
// The invented 125.00 open is a favorable gap: max(125.50, 125.00) = 125.50,
// so the same placement snapshot admits the fill, with 159 lots at 125.00.
void test_case_200pct_margin50_fills() {
    std::printf("-- synthetic 200%% at margin 50: fills 159 @125.00 --\n");
    Probe eng(10000.0, 200.0, 50.0, false);
    eng.script = "L.C.";
    std::vector<Bar> bars = {
        flat_bar(1000, 125.50),                          // L placed
        mk_bar(2000, 125.00, 126.00, 124.75, 125.50),       // fills @125.00
        flat_bar(3000, 125.50),                          // close placed
        flat_bar(4000, 125.50),                          // close fills
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.default_entries_pending_after_call == 1);
    CHECK(eng.default_entries_with_snapshot == 1);       // in scope: pct > 100
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).entry_id == "L");
        CHECK_NEAR(eng.get_trade(0).qty, 159.0, 1e-9); // floor(20,000 / 125.50)
        CHECK_NEAR(eng.get_trade(0).entry_price, 125.00, 1e-9);
    }
}

// Exactly 100% is byte-identical: no affordability snapshot is attached (the
// order stays on the KI-54 / gap-reject family) and the all-in entry fills as
// before: floor(10,000 / 125.50) = 79 lots at the favorable 125.00 open.
void test_case_100pct_control_unchanged() {
    std::printf("-- synthetic 100%% control: no snapshot, fills 79 --\n");
    Probe eng(10000.0, 100.0, 100.0, false);
    eng.script = "L...";
    std::vector<Bar> bars = {
        flat_bar(1000, 125.50),
        mk_bar(2000, 125.00, 126.00, 124.75, 125.50),
        flat_bar(3000, 125.50),
        flat_bar(4000, 125.50),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.default_entries_pending_after_call == 1);
    CHECK(eng.default_entries_with_snapshot == 0);       // out of scope
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 79.0, 1e-9); // floor(10,000 / 125.50)
    CHECK(eng.trade_count() == 0);
}

// 150% at margin 50 is 75% of equity: pct > 100 is not a blanket decline,
// the rule is the notional. floor(15,000 / 100) = 150 lots * 100 * 0.5 =
// 7,500 <= 10,000 -> fills.
void test_case_150pct_margin50_fills() {
    std::printf("-- synthetic 150%% at margin 50: 75%% of equity fills --\n");
    Probe eng(10000.0, 150.0, 50.0, false);
    eng.script = "L..";
    std::vector<Bar> bars = {
        flat_bar(1000, 100.00), flat_bar(2000, 100.00), flat_bar(3000, 100.00),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_size(), 150.0, 1e-9); // floor(15,000 / 100)
}

// The fill half: floor(20,000 / 100) = 200 lots at margin 50.
// Placement costs 200 * 100 * 0.5 = 10,000 (exact tie, admitted).
// A 100.25 open costs 10,025 > 10,000, so no fill or trade row;
// a favorable 99.50 open keeps max(close, fill) = 100 and admits 200 lots.
// POOC fills at the 100 close tie before the adverse next-open gap.
void test_case_200pct_margin50_gap_up_declined_at_fill() {
    std::printf("-- synthetic 200%% at margin 50: gap-up fill declined, gap-down fills --\n");
    {
        Probe eng(10000.0, 200.0, 50.0, false);
        eng.script = "L..";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.00),
            mk_bar(2000, 100.25, 101.00, 99.50, 100.00),    // 200 * 100.25 * .5 = 10,025 > 10,000
            flat_bar(3000, 100.00),
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
            flat_bar(1000, 100.00),
            mk_bar(2000, 99.50, 100.50, 99.00, 100.00),
            flat_bar(3000, 100.00),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 200.0, 1e-9); // floor(20,000 / 100)
    }
    {
        Probe eng(10000.0, 200.0, 50.0, false);
        eng.set_process_orders_on_close(true);
        eng.script = "L..";
        std::vector<Bar> bars = {
            flat_bar(1000, 100.00),
            mk_bar(2000, 100.25, 101.00, 99.50, 100.00),
            flat_bar(3000, 100.00),
        };
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.position_side_ == PositionSide::LONG);
        CHECK_NEAR(eng.position_size(), 200.0, 1e-9); // floor(20,000 / 100)
    }
}

// Reversal at 200% / margin 100: an explicit 50-lot long at 100 costs
// 5,000 <= 10,000. At close 104, MTM = 10,000 + 50 * (104 - 100) = 10,200.
// The default short sizes floor(2 * 10,200 / 104) = 196 lots; cost 20,384
// exceeds MTM, so the ENTRY leg is declined at placement. The close-only
// order survives and closes all 50 lots at the next 104 open under id "S".
void test_case_200pct_reversal_close_leg_only() {
    std::printf("-- synthetic 200%% reversal: close leg executes, no new entry --\n");
    Probe eng(10000.0, 200.0, 100.0, false);
    eng.entry_qty_ = 50.0;
    eng.script = "l.S...";
    std::vector<Bar> bars = {
        flat_bar(1000, 100.00),      // l placed
        flat_bar(2000, 100.00),      // 50 * 100 = 5,000; explicit long fills
        flat_bar(3000, 104.00),      // S placed: 196 * 104 = 20,384 > 10,200 -> close-only
        flat_bar(4000, 104.00),      // close leg fills
        flat_bar(5000, 104.00),
        flat_bar(6000, 104.00),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.default_entries_pending_after_call == 1);   // close-only survives
    CHECK(eng.position_side_ == PositionSide::FLAT);      // pre-fix: SHORT 196
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).qty, 50.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).exit_price, 104.00, 1e-9);
    }
    CHECK(eng.trades_with_entry_id("S") == 0);
}

// Reversal declined at FILL: 50 long lots held at 100, so MTM is 10,000.
// The 200% short sizes floor(20,000 / 100) = 200 lots; margin-50 cost is
// 200 * 100 * 0.5 = 10,000 (tie admitted at placement). A 102 open costs
// 200 * 102 * 0.5 = 10,200 > snapshot 10,000, so only the 50-lot close fills.
void test_case_200pct_reversal_declined_at_fill_closes_only() {
    std::printf("-- synthetic 200%% reversal declined at fill: close leg only --\n");
    Probe eng(10000.0, 200.0, 50.0, false);
    eng.entry_qty_ = 50.0;
    eng.script = "l.S...";
    std::vector<Bar> bars = {
        flat_bar(1000, 100.00),
        flat_bar(2000, 100.00),
        flat_bar(3000, 100.00),                          // S placed (tie)
        mk_bar(4000, 102.00, 103.00, 101.00, 102.00),       // 200 * 102 * .5 = 10,200 > 10,000
        flat_bar(5000, 102.00),
        flat_bar(6000, 102.00),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::FLAT);
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(eng.get_trade(0).is_long);
        CHECK(eng.get_trade(0).exit_id == "S");
        CHECK_NEAR(eng.get_trade(0).qty, 50.0, 1e-9);
        CHECK_NEAR(eng.get_trade(0).exit_price, 102.00, 1e-9);
    }
    CHECK(eng.trades_with_entry_id("S") == 0);
}

}  // namespace

int main() {
    std::printf("--- percent_equity_affordability ---\n");
    test_case_200pct_margin100_declined();
    test_case_200pct_margin50_fills();
    test_case_100pct_control_unchanged();
    test_case_150pct_margin50_fills();
    test_case_200pct_margin50_gap_up_declined_at_fill();
    test_case_200pct_reversal_close_leg_only();
    test_case_200pct_reversal_declined_at_fill_closes_only();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
