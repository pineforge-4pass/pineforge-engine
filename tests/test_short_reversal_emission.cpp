/*
 * test_short_reversal_emission.cpp — KI-72 short-decline emission corruption.
 *
 * Pins the fix for the emission/accounting split found during the KI-57 fix
 * cycle (PARK-DOSSIER D1a): a default-sized percent_of_equity reversal frozen
 * at NON-POSITIVE sizing equity (realized + open PnL underwater past the whole
 * account — reachable when a held SHORT's unbounded adverse excursion drives
 * equity negative while its reversal keeps being declined) produced a NEGATIVE
 * frozen_default_qty (apply_qty_step returns qty UNFLOORED for qty <= 0). The
 * legacy gate ran only for sizing_equity > 0, so the negative-equity order fell
 * through to the fill kernel and opened a NEGATIVE-qty position; every close of
 * it then emitted a negative-qty trade row that flipped the exported PnL sign
 * and blew the cumulative-PnL column, while the realized net_profit_sum_ stayed
 * healthy — the emission/accounting split. On almesned with symmetric-scope
 * KI-57 zero-tolerance this collapsed match 90.8% -> 53.4%, EVERY exported qty
 * negative, cumulative -122k.
 *
 * The fix declines such a non-positive-qty open CLEANLY (no fill, no trade row),
 * symmetric on both sides — the exact counterpart of a declined long. It fires
 * ONLY in the bankrupt regime, so every solvent path is byte-unchanged.
 *
 * These tests are the stash-cycle REDs base: A1/A2 (the negative-equity reversal
 * emits corrupt rows) are RED on baseline 6abebad and GREEN with the fix; the
 * controls C1/C2 (solvent reversal opens normally; solvent flat open unchanged)
 * pass in BOTH states.
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

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Scripted all-in reversal probe. margin_call OFF so a held short can ride
// deeply underwater (its unbounded adverse excursion drives realized + open
// equity NEGATIVE) — the exact state that used to size a reversal negative.
class RevProbe : public BacktestEngine {
public:
    RevProbe(double capital, double qty_step, bool mc) {
        initial_capital_ = capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        pyramiding_ = 1;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        qty_step_ = qty_step;
        set_margin_call_enabled(mc);
    }
    std::string script;   // 'S' short, 'L' long, '.' nothing (one char per bar)
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ < 0 || bar_index_ >= (int)script.size()) return;
        switch (script[bar_index_]) {
            case 'S': strategy_entry("S", false); break;
            case 'L': strategy_entry("L", true); break;
            default: break;
        }
    }
    double trade_size(int i) const { return closed_trade_size(i); }
    double trade_pnl(int i) const { return closed_trade_profit(i); }
    int worst_negative_qty_rows() const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i)
            if (closed_trade_size(i) < 0.0) ++n;
        return n;
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::net_profit_sum_;
    using BacktestEngine::initial_capital_;
};

// A1. NEGATIVE-EQUITY REVERSAL declined cleanly — no corrupt rows. A short rides
// underwater until realized+openPnL equity is negative; the short->long reversal
// signalled there froze a NEGATIVE default qty. Baseline OPENS a negative-qty
// long, then the next flip emits a negative-qty trade row (RED). The fix DECLINES
// the non-positive-qty open, leaving the short in place: zero negative rows.
//   short 100@100; ramp 150/200/260 -> eq 10000+(100-260)*100 = -6000 (NEG);
//   L reversal freezes qty = calc_qty(260) on -6000 = -23.08 <= 0 -> DECLINE.
void test_negative_equity_reversal_declined_clean() {
    std::printf("-- A1: negative-equity reversal declined, no negative-qty rows --\n");
    RevProbe eng(10000.0, /*qty_step=*/0.0001, /*mc=*/false);
    eng.script = "S....L.S.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // S placed
        mk_bar(2000, 100, 100, 100, 100),   // short fills @100
        mk_bar(3000, 150, 150, 150, 150),   // eq 5000
        mk_bar(4000, 200, 200, 200, 200),   // eq 0
        mk_bar(5000, 260, 260, 260, 260),   // eq -6000 (NEG)
        mk_bar(6000, 260, 260, 260, 260),   // L reversal signalled at neg equity
        mk_bar(7000, 260, 260, 260, 260),   // reversal would fill here
        mk_bar(8000, 260, 260, 260, 260),   // S re-signalled
        mk_bar(9000, 260, 260, 260, 260),   // would flip the neg-qty long -> emit
    };
    eng.run(bars.data(), (int)bars.size());
    // The whole cluster of reversals is refused; the underwater short rides on.
    CHECK(eng.worst_negative_qty_rows() == 0);   // RED on baseline (1 neg row)
    CHECK(eng.position_side_ == PositionSide::SHORT);
    CHECK(eng.position_qty_ > 0.0);              // never a negative-qty position
    // Every emitted trade row carries a POSITIVE size (Pine/TV magnitude column).
    for (int i = 0; i < eng.trade_count(); ++i) CHECK(eng.trade_size(i) >= 0.0);
}

// A2. EMISSION/ACCOUNTING split closed: the exported per-trade PnL (sum) agrees
// with the realized net_profit_sum_. Under the corruption the negative-qty rows
// flipped the exported PnL while net_profit_sum_ stayed healthy; after the fix
// there are no such rows, so the two agree.
void test_exported_pnl_matches_realized() {
    std::printf("-- A2: exported per-trade PnL sum == realized net_profit_sum_ --\n");
    RevProbe eng(10000.0, /*qty_step=*/0.0001, /*mc=*/false);
    eng.script = "S....L.S.L.";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),
        mk_bar(2000, 100, 100, 100, 100),   // short fills @100
        mk_bar(3000, 150, 150, 150, 150),
        mk_bar(4000, 200, 200, 200, 200),
        mk_bar(5000, 260, 260, 260, 260),   // eq negative
        mk_bar(6000, 260, 260, 260, 260),   // L reversal (declined)
        mk_bar(7000, 260, 260, 260, 260),
        mk_bar(8000, 260, 260, 260, 260),   // S (same dir, noop)
        mk_bar(9000, 100, 100, 100, 100),   // price recovers
        mk_bar(10000, 100, 100, 100, 100),  // L reversal (now solvent) fills
        mk_bar(11000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    double exported_sum = 0.0;
    for (int i = 0; i < eng.trade_count(); ++i) exported_sum += eng.trade_pnl(i);
    CHECK(eng.worst_negative_qty_rows() == 0);
    CHECK_NEAR(exported_sum, eng.net_profit_sum_, 1e-6);
}

// C1. CONTROL — a SOLVENT reversal is unaffected: at positive equity the
// short->long flip opens the full all-in long and emits a clean positive-qty
// close of the short. (Passes in BOTH stash states — the fix is a no-op when
// solvent.)
void test_solvent_reversal_opens_normally() {
    std::printf("-- C1: solvent reversal opens normally (control) --\n");
    RevProbe eng(10000.0, /*qty_step=*/1.0, /*mc=*/false);
    eng.script = "S..L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // S placed
        mk_bar(2000, 100, 100, 100, 100),   // short fills @100 (eq 10000)
        mk_bar(3000,  99,  99,  99,  99),   // small favorable move, still solvent
        mk_bar(4000,  99,  99,  99,  99),   // L reversal signalled
        mk_bar(5000,  99,  99,  99,  99),   // reversal fills -> LONG opens
        mk_bar(6000,  99,  99,  99,  99),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);   // the flip happened
    CHECK(eng.position_qty_ > 0.0);
    CHECK(eng.trade_count() >= 1);                      // short closed cleanly
    CHECK(eng.worst_negative_qty_rows() == 0);
}

// C2. CONTROL — a SOLVENT true-flat all-in long open is unchanged: opens the
// full floor lot, one entry, no spurious decline. Pins that the KI-72 branch
// does not touch ordinary flat opens.
void test_solvent_flat_open_unchanged() {
    std::printf("-- C2: solvent flat all-in open unchanged (control) --\n");
    RevProbe eng(10000.0, /*qty_step=*/1.0, /*mc=*/false);
    eng.script = "L..";
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),   // L placed
        mk_bar(2000, 100, 100, 100, 100),   // long fills @100
        mk_bar(3000, 100, 100, 100, 100),
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.position_side_ == PositionSide::LONG);
    CHECK_NEAR(eng.position_qty_, 100.0, 1e-9);   // full all-in floor lot
    CHECK(eng.trade_count() == 0);
}

}  // namespace

int main() {
    std::printf("--- short_reversal_emission (KI-72) ---\n");
    test_negative_equity_reversal_declined_clean();
    test_exported_pnl_matches_realized();
    test_solvent_reversal_opens_normally();
    test_solvent_flat_open_unchanged();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
