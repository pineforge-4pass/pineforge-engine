/*
 * test_margin_call.cpp — verify TradingView forced-liquidation (margin call).
 *
 * Covers the principal behaviours of process_margin_call /
 * margin_liquidation_price:
 *
 *   A. A 100%-equity SHORT held through an adverse (rising) move is force-
 *      liquidated. At least one "Margin call" exit is produced; the first one
 *      fills at the bar's adverse extreme (HIGH) and closes the documented 4x
 *      of the margin shortfall (capped at the full position). The reported
 *      margin_liquidation_price equals the closed-form formula while open.
 *
 *   B. A LONG at the default 100% margin has no adverse-price liquidation
 *      (the formula denominator margin/100 - direction = 0). A sub-lot
 *      opening affordability overage is held even through a later crash.
 *
 *   C. A one-lot-or-larger opening affordability shortfall is trimmed on the
 *      entry bar using entry affordability and exit-side fill semantics.
 *
 *   D. A LEVERAGED long (margin_long = 20 => 5x) IS liquidated when price falls
 *      far enough; the forced exit fills at the bar's adverse extreme (LOW).
 *
 *   E. The margin-call emulator can be switched off (set_margin_call_enabled
 *      false); the underwater short is then held with no forced exit.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) < tol;
}

namespace {

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c, double v) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = v; b.timestamp = ts;
    return b;
}

// Thin base exposing the protected closed-trade accessors / liquidation price
// for the test harness (these are protected on BacktestEngine, accessible only
// from subclasses).
class MCEngine : public BacktestEngine {
public:
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    int entry_bar(int i) const { return closed_trade_entry_bar_index(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
    double liq_price() const { return margin_liquidation_price(); }
    bool opening_pending() const { return opening_affordability_pending_; }
    bool opening_eligible() const { return opening_affordability_eligible_; }
    double opening_raw_base() const {
        return opening_affordability_raw_fill_base_;
    }
    int live_entry_count() const { return position_entry_count_; }
};

// ---- A: 100%-equity short force-liquidated by a rising market --------------

class ShortLiqProbe : public MCEngine {
public:
    bool disable_mc_ = false;
    explicit ShortLiqProbe(bool disable_mc = false, double qty_step = 0.0) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;          // size the short at 100% of equity
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;               // 1x, default TV margin
        process_orders_on_close_ = true;     // market entry fills at bar close
        disable_mc_ = disable_mc;
        qty_step_ = qty_step;                // 0 = no lot quantization
        if (disable_mc_) set_margin_call_enabled(false);
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            // Short the whole account; never exit. Fills at bar0 close = 100.
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }
    }
};

static void test_short_margin_call() {
    std::printf("test_short_margin_call\n");

    // bar0 entry @ close=100 (qty = 1000/100 = 10, notional 1000 = equity).
    //   liqPrice (short, 100% margin) = ((1000/10) + 100) / 2 = 100.
    // bar1 small rise: high=105 > liq=100 -> partial 4x liquidation @ high=105.
    //   equity@105 = 1000 - (105-100)*10 = 950; reqMargin@105 = 10*105 = 1050.
    //   qmin = 10 - 950/105 = 0.952381; 4x = 3.809524 (< 10) -> partial fill.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: short fills @100
        mk_bar(2000, 101.0, 105.0, 100.5, 104.0, 1.0),  // 1: high 105 -> margin call
        mk_bar(3000, 104.0, 130.0, 103.0, 128.0, 1.0),  // 2: high 130 -> further call
        mk_bar(4000, 128.0, 140.0, 127.0, 139.0, 1.0),  // 3: keep rising
    };

    ShortLiqProbe eng;
    // Margin-call price while the full 10@100 short is open.
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() >= 1);
    // Every closed trade on this no-exit strategy must be a forced liquidation.
    bool all_margin = true;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) != std::string("Margin call"))
            all_margin = false;
    }
    CHECK(all_margin);

    // First liquidation: fills at bar1's adverse extreme (high = 105) and
    // closes ~3.8095 contracts (4x the shortfall), leaving the position open.
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(near(eng.trade_size(0), 3.80952381, 1e-4));
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
}

static void test_margin_liquidation_price_formula() {
    std::printf("test_margin_liquidation_price_formula\n");

    // Re-run only the entry bar (no adverse move yet) and read the formula
    // before any liquidation: short 10 @ 100, equity 1000, margin 100% ->
    // liqPrice = ((1000/10) + 100) / 2 = 100.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: short fills @100
        mk_bar(2000, 100.0, 100.0,  99.5, 100.0, 1.0),  // 1: no breach (high == liq)
    };
    ShortLiqProbe eng;
    eng.run(bars.data(), (int)bars.size());
    // No adverse move above 100 -> no margin call, position still open.
    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.liq_price(), 100.0));
}

static void test_short_margin_call_disabled() {
    std::printf("test_short_margin_call_disabled\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 101.0, 105.0, 100.5, 104.0, 1.0),
        mk_bar(3000, 104.0, 200.0, 103.0, 199.0, 1.0),  // huge adverse move
    };
    ShortLiqProbe eng(/*disable_mc=*/true);
    eng.run(bars.data(), (int)bars.size());
    // With the emulator off the underwater short is simply held: no exits.
    CHECK(eng.trade_count() == 0);
}

// ---- A': lot quantization floors each forced-liquidation lot to qty_step ----

// Returns true when |x| is an integer multiple of step (within tol).
static bool is_multiple_of(double x, double step, double tol = 1e-9) {
    if (step <= 0.0) return false;
    double n = std::round(x / step);
    return std::fabs(x - n * step) <= tol;
}

static void test_short_margin_call_qty_step() {
    std::printf("test_short_margin_call_qty_step\n");

    // Same scenario as test_short_margin_call. The shortfall (minimum restore
    // qty) is 3.80952381/4 = 0.95238095 contracts. TradingView floors the
    // restore qty to the lot step BEFORE the 4x over-liquidation (KI-31), so:
    //   floor(0.95238095 / 0.5) * 0.5 = 0.5, then * 4 = 2.0 (an exact step
    // multiple). Flooring the 4x product instead (the old bug) gave 3.5 and
    // desynced multi-nibble cascades from TV. The exit price is unchanged
    // (bar1 high = 105).
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: short fills @100
        mk_bar(2000, 101.0, 105.0, 100.5, 104.0, 1.0),  // 1: high 105 -> margin call
        mk_bar(3000, 104.0, 130.0, 103.0, 128.0, 1.0),  // 2: high 130 -> further call
        mk_bar(4000, 128.0, 140.0, 127.0, 139.0, 1.0),  // 3: keep rising
    };

    const double step = 0.5;
    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/step);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() >= 1);
    // First quantized lot: 4 * floor(shortfall/step)*step = 4 * 0.5 = 2.0,
    // an exact multiple of the 0.5 step (floor-before-4x per KI-31).
    CHECK(near(eng.trade_size(0), 2.0));
    CHECK(is_multiple_of(eng.trade_size(0), step));
    // Quantization never enlarges the lot: floored <= unquantized 3.80952381.
    CHECK(eng.trade_size(0) <= 3.80952381 + 1e-9);
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(eng.exit_comment(0) == std::string("Margin call"));

    // Negative sentinel for the 1x-long fix: the established short cascade
    // stays exactly two forced rows, including the residual close at bar2 HIGH.
    CHECK(eng.trade_count() == 2);
    CHECK(near(eng.trade_size(1), 8.0));
    CHECK(near(eng.exit_price(1), 130.0));
    CHECK(eng.exit_comment(1) == std::string("Margin call"));

    // Every partial (non-final) forced lot must be a step multiple. The final
    // exit closes whatever residual remains (the position size itself is not a
    // step multiple, so only the intermediate nibbles are checked).
    int partial_checked = 0;
    for (int i = 0; i + 1 < eng.trade_count(); ++i) {
        CHECK(is_multiple_of(eng.trade_size(i), step));
        ++partial_checked;
    }
    CHECK(partial_checked >= 1);

    // Teeth: with qty_step = 0 the same first lot is the UNQUANTIZED 3.80952381,
    // which is NOT a multiple of 0.5 — proving the assertion above can fail.
    ShortLiqProbe raw(/*disable_mc=*/false, /*qty_step=*/0.0);
    raw.run(bars.data(), (int)bars.size());
    CHECK(near(raw.trade_size(0), 3.80952381, 1e-4));
    CHECK(!is_multiple_of(raw.trade_size(0), step));
}

// ---- B: long at 100% margin is never liquidated ----------------------------

class LongNoLiqProbe : public MCEngine {
public:
    LongNoLiqProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;             // 1x -> denominator (1 - 1) = 0 -> na
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, kNaN);
    }
};

static void test_long_100pct_margin_no_call() {
    std::printf("test_long_100pct_margin_no_call\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0,  99.0, 100.0, 1.0),  // 0: long fills @100
        mk_bar(2000, 100.0, 100.0,  10.0,  20.0, 1.0),  // 1: -90% crash
        mk_bar(3000,  20.0,  21.0,   1.0,   2.0, 1.0),  // 2: keeps crashing
    };
    LongNoLiqProbe eng;
    eng.run(bars.data(), (int)bars.size());
    // A long at 100% margin can never be margin-called: position is held.
    CHECK(eng.trade_count() == 0);
    // The accessor must report na (no liquidation price exists).
    CHECK(std::isnan(eng.liq_price()));
}

// A default 100%-of-equity MARKET order placed and filled from true flat.
// A ZERO-commission fill is admitted only within one lot of the frozen sizing
// notional: a larger gap-up is REJECTED at fill and silently dropped
// (design-cntvxiao-gap-reject). A COMMISSIONED fill is never gap-rejected (its
// opening fee is nonzero) and instead keeps the KI-61 fill-then-entry-bar
// affordability-trim path.
class FrozenAllInFlatLongProbe : public MCEngine {
public:
    explicit FrozenAllInFlatLongProbe(double commission_percent) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_percent;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, kNaN);
    }
};

// Zero commission + an above-lot gap-up: the frozen 10-lot notional at the
// 120 fill (1200) exceeds the 1000 sizing equity by more than one lot (one lot
// = qty_step*fill = 120), so TV REJECTS the entry at fill and it is silently
// dropped — the account stays FLAT, no trade row. (Pre-gap-reject the engine
// HELD the 10-lot fill exempt from the affordability trim; the rejection is
// design-cntvxiao-gap-reject.)
static void test_zero_cost_frozen_all_in_true_flat_gap_is_rejected() {
    std::printf("test_zero_cost_frozen_all_in_true_flat_gap_is_rejected\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    FrozenAllInFlatLongProbe eng(/*commission_percent=*/0.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 0.0));   // was 10 (held); now dropped
    CHECK(eng.position_size() == 0.0);
}

static void test_commissioned_frozen_all_in_true_flat_gap_is_eligible() {
    std::printf("test_commissioned_frozen_all_in_true_flat_gap_is_eligible\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    FrozenAllInFlatLongProbe eng(/*commission_percent=*/10.0);
    eng.run(bars.data(), (int)bars.size());

    // Signal sizing reserves the 10% entry fee: floor(1000/1.1/100)=9.
    // At the 120 fill, margin plus the 108 fee is unaffordable. Restoring
    // needs 1.566... lots, floored to one then multiplied by four.
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(eng.entry_bar(0) == 1);
    CHECK(eng.exit_bar(0) == 1);
    CHECK(near(eng.entry_price(0), 120.0));
    CHECK(near(eng.exit_price(0), 120.0));
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(near(eng.position_size(), 5.0));
}

// The short closes at zero PnL immediately before the long is placed, so both
// placement and fill observe FLAT. Direct same-on_bar close provenance (not a
// trade-count/PnL heuristic) must still identify the paired reentry; otherwise
// its next-open gap would be mistaken for the true-flat exemption.
class PairedCloseDefaultLongProbe : public MCEngine {
public:
    PairedCloseDefaultLongProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            // Close immediately first, so the reentry is placed from an
            // actually FLAT engine state. Its same-on_bar paired-close
            // provenance must still exclude it from the true-flat exemption.
            strategy_close("S", "paired close", kNaN, kNaN,
                           /*immediately=*/true);
            strategy_entry("L", true, kNaN, kNaN, kNaN);
        }
    }
};

static void test_paired_short_close_default_long_gap_remains_eligible() {
    std::printf("test_paired_short_close_default_long_gap_remains_eligible\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    PairedCloseDefaultLongProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(eng.entry_bar(1) == 2);
    CHECK(eng.exit_bar(1) == 2);
    CHECK(near(eng.entry_price(1), 120.0));
    CHECK(near(eng.exit_price(1), 120.0));
    CHECK(near(eng.trade_size(1), 4.0));
    CHECK(near(eng.position_size(), 6.0));
}

// ---- B'/C: 1x-long opening affordability is lot-floored and entry-priced ---

class LongOverAllocProbe : public MCEngine {
public:
    explicit LongOverAllocProbe(double qty_step,
                                double commission_percent = 0.0,
                                bool process_on_close = false,
                                int slippage_ticks = 0,
                                double mintick = 0.01,
                                double account_fx = 1.0,
                                double pointvalue = 1.0,
                                double qty = 10.0) : qty_(qty) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = qty;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commission_percent;
        margin_long_ = 100.0;             // 1x -> denominator (1 - 1) = 0 -> na
        process_orders_on_close_ = process_on_close;
        qty_step_ = qty_step;
        slippage_ = slippage_ticks;
        syminfo_mintick_ = mintick;
        account_currency_fx_ = account_fx;
        syminfo_.pointvalue = pointvalue;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, qty_);
    }
private:
    double qty_;
};

// Repurposed from the KI-61 "sublot overage held" fixture (design-explicit-qty-
// fill-admission). This is an EXPLICIT-qty all-in true-flat MARKET entry
// (strategy.entry with qty=10 == equity/close) whose next-bar fill gaps
// ADVERSELY to 110: notional 10*110 = 1100 overshoots equity 1000. TV DECLINES
// it outright with ZERO slack — the frozen path's one-lot lot-floor slack does
// NOT apply to explicit qty — so the pre-fix "held 10 via lot-floor dust"
// outcome is dead: no fill, no rows, no margin call. Evidence:
// data/probes/pf-probe-allin-floor-comm0 (4,740 from-flat attempts; decline iff
// fill notional > equity, commission-independent, zero slack). The KI-61
// lot-floored opening-affordability trim these fixtures once exercised is still
// pinned by the frozen/default-sized path (test_commissioned_frozen_all_in_
// true_flat_gap_is_eligible for commissioned-adverse admit+trim; the frozen
// exemption tests for the sub-lot held case) plus test_explicit_qty_fill_
// admission's GREEN-D.
static void test_explicit_all_in_zero_comm_adverse_gap_declined() {
    std::printf("test_explicit_all_in_zero_comm_adverse_gap_declined\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: signal @ close 100
        mk_bar(2000, 110.0, 112.0,  50.0,  90.0, 1.0),  // 1: gap 110 -> 1100 > 1000 DECLINE
        mk_bar(3000,  90.0,  91.0,   1.0,   2.0, 1.0),  // 2: later crash: nothing held
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 10
    CHECK(std::isnan(eng.liq_price()));
}

// Repurposed from the KI-61 "lot trim uses entry affordability" fixture. An
// EXPLICIT-qty all-in true-flat MARKET entry (qty=10 == equity/close), commission
// 4%, fill gaps ADVERSELY to 120: the NOTIONAL 10*120 = 1200 alone overshoots
// equity 1000 (the fee is irrelevant to the predicate). Commission-scoping is
// DEAD (data/probes/pf-probe-allin-floor-comm0 is comm=0 and still declines), so
// TV DECLINES this too — the pre-fix "fill 10@120 then 4x entry-bar trim to hold
// 2" outcome is dead. No fill, no Margin-call rows. The commissioned admit+trim
// KI-61 semantics remain pinned by the FROZEN path
// (test_commissioned_frozen_all_in_true_flat_gap_is_eligible).
static void test_explicit_all_in_commissioned_adverse_gap_declined() {
    std::printf("test_explicit_all_in_commissioned_adverse_gap_declined\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 122.0, 100.0, 110.0, 1.0),   // gap 120 -> 1200 > 1000 DECLINE
        mk_bar(3000, 110.0, 111.0,  10.0,  20.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0, /*commission_percent=*/4.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);            // pre-fix: 1 (fill + Margin-call trim)
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 2
    CHECK(std::isnan(eng.liq_price()));
}

// Repurposed from the KI-61 "trim without qty_step" fixture. EXPLICIT-qty all-in
// true-flat MARKET entry (qty=10 == equity/close), zero commission, qty_step=0
// (continuous mode), fill gaps ADVERSELY to 110: notional 1100 > equity 1000.
// This is exactly test_explicit_qty_fill_admission RED-1's class (zero comm,
// zero slack, adverse gap), so TV DECLINES — the pre-fix fractional entry-bar
// trim is dead. No fill, no rows.
static void test_explicit_all_in_zero_comm_no_qty_step_declined() {
    std::printf("test_explicit_all_in_zero_comm_no_qty_step_declined\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 112.0, 50.0,  90.0, 1.0),   // gap 110 -> 1100 > 1000 DECLINE
        mk_bar(3000,  90.0,  91.0,  1.0,   2.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/0.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);            // pre-fix: 1 (fill + fractional trim)
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 10 - trim
    CHECK(std::isnan(eng.liq_price()));
}

// Repurposed from the KI-61 "combines fx/pointvalue/commission" fixture. The
// explicit-qty fill-admission predicate carries the SAME pv/fx/margin factors as
// KI-61 (|qty|*slipped_fill*pv*fx*margin/100). Signal-time admission is exact
// (5*10*pv10*fx2 == 1000 == equity); the ADVERSE fill at 12 makes the notional
// 5*12*10*2 = 1200 > equity 1000, so TV DECLINES (commission 10% excluded from
// the predicate). Pins that the decline arithmetic combines pv, fx, and margin
// exactly like the KI-61 trim it replaces here. No fill, no rows.
static void test_explicit_all_in_fx_pointvalue_commission_declined() {
    std::printf("test_explicit_all_in_fx_pointvalue_commission_declined\n");
    std::vector<Bar> bars = {
        // Signal-time admission is exact: 5 * 10 * pv10 * fx2 == 1000.
        mk_bar(1000, 10.0, 10.0,  9.0, 10.0, 1.0),
        mk_bar(2000, 12.0, 13.0,  8.0, 11.0, 1.0),   // 5*12*10*2 = 1200 > 1000 DECLINE
        mk_bar(3000, 11.0, 12.0,  1.0,  2.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0, /*commission_percent=*/10.0,
                           /*process_on_close=*/false, /*slippage_ticks=*/0,
                           /*mintick=*/0.01, /*account_fx=*/2.0,
                           /*pointvalue=*/10.0, /*qty=*/5.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);            // pre-fix: 1 (fill + trim to hold 1)
    CHECK(near(eng.position_size(), 0.0));    // pre-fix: held 1
    CHECK(std::isnan(eng.liq_price()));
}

static void test_long_100pct_margin_trim_process_orders_on_close() {
    std::printf("test_long_100pct_margin_trim_process_orders_on_close\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 101.0, 10.0,  20.0, 1.0),
    };
    LongOverAllocProbe eng(/*qty_step=*/1.0, /*commission_percent=*/12.0,
                           /*process_on_close=*/true);
    eng.run(bars.data(), (int)bars.size());

    // Entry commission makes q_restore=1.2, floors to one and trims four on
    // bar0 itself. The generic "no adverse path after a close fill" rule must
    // not suppress this non-price affordability action.
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(eng.entry_bar(0) == 0);
    CHECK(eng.exit_bar(0) == 0);
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(near(eng.exit_price(0), 100.0));
    CHECK(near(eng.position_size(), 6.0));
}

class LongPricedOverAllocProbe : public MCEngine {
public:
    enum class Kind { Stop, Limit };

    explicit LongPricedOverAllocProbe(Kind kind) : kind_(kind) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
        slippage_ = 2;
        syminfo_mintick_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        if (kind_ == Kind::Stop) {
            strategy_entry("L", true, kNaN, /*stop=*/120.2, /*qty=*/10.0);
        } else {
            strategy_entry("L", true, /*limit=*/120.8, kNaN, /*qty=*/10.0);
        }
    }

private:
    Kind kind_;
};

static void test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip() {
    std::printf("test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 130.0, 90.0, 115.0, 1.0),
        mk_bar(3000, 115.0, 116.0, 10.0,  20.0, 1.0),
    };
    LongPricedOverAllocProbe eng(LongPricedOverAllocProbe::Kind::Stop);
    eng.run(bars.data(), (int)bars.size());

    // KI-62 STAGE 3 (margin fill-time admission): this buy-stop 120.2 is
    // OVER-ALLOCATED — qty 10 on a $1,000 account, and the margin gate costs it
    // at the FILL BAR'S OPEN (110): required 10*110*100% = 1100 > equity 1000 ->
    // DECLINE. The order never fills, so the old KI-61 1x-long dust-trim (which
    // used to report fill@123 then trim to size 4) does NOT fire — the
    // "declined fills must not fire the dust-trim" reconciliation.
    //
    // TV declines over-allocated stops too: cross-confirmed by
    // pf-probe-ki65-dual-entry-precedence, whose UQ=1,000,000 (>=1000x
    // over-notional) stop cells decline under the identical rule, lifting its
    // canonical TV match 93.8% -> 100.0%. The LIMIT sibling below is UNAFFECTED
    // (the gate is stop-entry-only) and still fills + trims.
    //
    // CAVEAT (register): the OVER-ALLOCATED FIXED-QTY class is UNPINNED by the
    // ki62 probe itself (which used all-in / marginal / fixed-small sizing). It
    // is a candidate future-probe cell; if any tier ever regresses tracing to a
    // strategy relying on the old admit-and-trim vs TV, this scopes back to
    // admit-then-nibble and the cell becomes a probe requirement.
    CHECK(eng.trade_count() == 0);            // declined at the fill-bar open
    CHECK(near(eng.position_size(), 0.0));    // nothing opened
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

static void test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip() {
    std::printf("test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 130.0, 131.0, 129.0, 130.0, 1.0),
        mk_bar(2000, 130.0, 134.0, 100.0, 110.0, 1.0),
        mk_bar(3000, 110.0, 111.0,  10.0,  20.0, 1.0),
    };
    LongPricedOverAllocProbe eng(LongPricedOverAllocProbe::Kind::Limit);
    eng.run(bars.data(), (int)bars.size());

    // buy limit 120.8 snaps favorably to entry 120 and receives no entry
    // slippage. The affordability trim is a broker market sell: raw matched
    // base 120.8 minus two ticks snaps down to 118.
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(near(eng.entry_price(0), 120.0));
    CHECK(near(eng.exit_price(0), 118.0));
    CHECK(eng.entry_bar(0) == 1);
    CHECK(eng.exit_bar(0) == 1);
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

class RawOpeningProbe : public MCEngine {
public:
    RawOpeningProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_order("RAW", true, /*qty=*/10.0);
    }
};

static void test_raw_order_fresh_open_captures_affordability() {
    std::printf("test_raw_order_fresh_open_captures_affordability\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 120.0, 125.0,  80.0, 110.0, 1.0),
    };
    RawOpeningProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 4.0));
    CHECK(near(eng.entry_price(0), 120.0));
    CHECK(near(eng.exit_price(0), 120.0));
    CHECK(near(eng.position_size(), 6.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

// ---- C': explicit opening-affordability lifecycle -------------------------

static int margin_call_rows(const MCEngine& eng) {
    int count = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) == std::string("Margin call")) ++count;
    }
    return count;
}

// A genuine accepted same-direction add is itself a post-fill affordability
// event. FIFO then drains the original lot and makes the mutable entry count
// equal one again; the event must survive because it came from the accepted
// add directly, not from reconstructing provenance from the remaining count.
class AcceptedAddFifoProbe : public MCEngine {
public:
    bool captured_after_open = false;
    bool eligible_after_add = false;
    bool eligible_after_fifo = false;
    int count_after_fifo = -1;

    AcceptedAddFifoProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        captured_after_open = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0);

        // A priced explicit entry bypasses the market-only signal admission
        // gate and is a genuine accepted append (10 -> 25), not a rejected
        // over-allocation attempt. It is immediately marketable at this close.
        strategy_entry("ADD", true, /*limit=*/100.0, kNaN, /*qty=*/15.0);
        process_pending_orders(current_bar_);
        eligible_after_add = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0);

        // FIFO removes the opening lot, leaving only ADD and restoring the
        // mutable position_entry_count_ to one. The add event must stay live.
        strategy_close("OPEN", "fifo drain", /*qty=*/10.0,
                       /*qty_percent=*/kNaN, /*immediately=*/true);
        count_after_fifo = position_entry_count_;
        eligible_after_fifo = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0);
    }
};

static void test_accepted_add_fifo_keeps_add_affordability_event() {
    std::printf("test_accepted_add_fifo_keeps_add_affordability_event\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    AcceptedAddFifoProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.captured_after_open);
    CHECK(eng.eligible_after_add);
    CHECK(eng.count_after_fifo == 1);  // cannot reconstruct from this count
    CHECK(eng.eligible_after_fifo);
    // The one-shot event is consumed at the end-of-bar margin pass.
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.trade_count() == 2);  // explicit FIFO close + margin call
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.trade_size(1), 15.0));
    CHECK(near(eng.position_size(), 0.0));
}

// A rejected same-direction attempt must not erase the fresh opening's state.
// Commission then makes the opening itself genuinely unaffordable, proving the
// preserved state remains actionable in the end-of-bar check.
class RejectedAddProbe : public MCEngine {
public:
    bool preserved_after_rejection = false;

    RejectedAddProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 1;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        strategy_entry("REJECTED_ADD", true, kNaN, kNaN, /*qty=*/1.0);
        process_pending_orders(current_bar_);  // rejected by pyramiding=1
        preserved_after_rejection = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0)
            && position_entry_count_ == 1
            && near(position_qty_, 10.0);
    }
};

static void test_rejected_add_preserves_opening_eligibility() {
    std::printf("test_rejected_add_preserves_opening_eligibility\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    RejectedAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.preserved_after_rejection);
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 8.0));
    CHECK(near(eng.position_size(), 2.0));
}

// A same-bar add whose requested quantity floors to zero has no accepted
// position effect. Its implementation currently appends a zero-qty roster
// element, so the opening-affordability lifecycle must key on positive added
// quantity rather than vector growth alone.
class ZeroQtyAddProbe : public MCEngine {
public:
    bool preserved_after_zero_add = false;

    ZeroQtyAddProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);

        // apply_qty_step(0.5) == 0 with qty_step=1: the fill kernel appends a
        // zero-qty bookkeeping lot but live position quantity stays exactly 10.
        strategy_entry("ZERO_ADD", true, kNaN, kNaN, /*qty=*/0.5);
        process_pending_orders(current_bar_);
        preserved_after_zero_add = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0)
            && near(position_qty_, 10.0);
    }
};

static void test_zero_qty_add_preserves_opening_eligibility() {
    std::printf("test_zero_qty_add_preserves_opening_eligibility\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ZeroQtyAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.preserved_after_zero_add);
    // The original opening remains actionable: its 20% entry commission gives
    // q_restore=2 lots, so the 4x rule still trims eight on the opening bar.
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 8.0));
    CHECK(near(eng.position_size(), 2.0));
}

// CASH_PER_ORDER charges once per accepted order, not once per bookkeeping
// row. A high-level add that floors to zero currently appends a zero-qty
// pyramid row; counting that row as a second fee crosses this deliberately
// chosen lot-floor boundary and manufactures a four-lot trim.
class ZeroQtyCashPerOrderAddProbe : public MCEngine {
public:
    bool preserved_after_zero_add = false;

    ZeroQtyCashPerOrderAddProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::CASH_PER_ORDER;
        commission_value_ = 60.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        strategy_entry("ZERO_ADD", true, kNaN, kNaN, /*qty=*/0.5);
        process_pending_orders(current_bar_);
        preserved_after_zero_add = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(position_qty_, 10.0);
    }
};

static void test_zero_qty_add_does_not_duplicate_cash_per_order_fee() {
    std::printf("test_zero_qty_add_does_not_duplicate_cash_per_order_fee\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ZeroQtyCashPerOrderAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    // One real $60 fee: q_min=(1000-(1000-60))/100=.6, floors to zero.
    // Charging the zero-qty row adds a phantom second fee: q_min=1.2,
    // floors to one, then the 4x rule incorrectly trims four contracts.
    CHECK(eng.preserved_after_zero_add);
    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 10.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

// A full close clears the state; a later RAW fresh opening in the same bar
// captures a new raw base and can receive its own affordability trim.
class FlatThenRawFreshProbe : public MCEngine {
public:
    bool first_captured = false;
    bool add_eligible = false;
    bool flat_cleared = false;
    bool raw_fresh_captured = false;

    FlatThenRawFreshProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("OPEN", true, kNaN, kNaN, /*qty=*/10.0);
        process_pending_orders(current_bar_);
        first_captured = opening_affordability_pending_
            && opening_affordability_eligible_;

        strategy_order("ADD", true, /*qty=*/15.0);
        process_pending_orders(current_bar_);
        add_eligible = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0);

        strategy_close_all();
        flat_cleared = position_side_ == PositionSide::FLAT
            && !opening_affordability_pending_
            && !opening_affordability_eligible_
            && std::isnan(opening_affordability_raw_fill_base_);

        strategy_order("RAW_FRESH", true, /*qty=*/12.0);
        process_pending_orders(current_bar_);
        raw_fresh_captured = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0);
    }
};

static void test_flat_clears_and_raw_fresh_reuses_state() {
    std::printf("test_flat_clears_and_raw_fresh_reuses_state\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    FlatThenRawFreshProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.first_captured);
    CHECK(eng.add_eligible);
    CHECK(eng.flat_cleared);
    CHECK(eng.raw_fresh_captured);
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
    CHECK(margin_call_rows(eng) == 1);
    CHECK(near(eng.position_size(), 4.0));
}

// Reversal is a fresh position cycle. The closing short's realized loss must
// be in the new long's opening-equity basis, and the raw reversal match must be
// captured for the broker-generated closing leg.
class ReversalOpeningProbe : public MCEngine {
public:
    ReversalOpeningProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 1;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            strategy_entry("L", true, kNaN, kNaN, /*qty=*/10.0);
        }
    }
};

static void test_reversal_captures_fresh_opening_state() {
    std::printf("test_reversal_captures_fresh_opening_state\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 120.0, 121.0,  80.0, 110.0, 1.0),
    };
    ReversalOpeningProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);  // short reversal close + long MC nibble
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.entry_price(1), 120.0));
    CHECK(near(eng.exit_price(1), 120.0));
    CHECK(near(eng.trade_size(1), 4.0));
    CHECK(near(eng.position_size(), 6.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

// A reused engine handle must clear the per-position state before on_bar of
// the next run. Run 1 deliberately ends with an open position whose one-shot
// event was consumed; run 2 observes a clean state before opening a new RAW
// position and must equal a fresh handle executing run 2 directly.
class ReuseOpeningProbe : public MCEngine {
public:
    bool second_mode = false;
    bool saw_clean_run_start = false;

    ReuseOpeningProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 10.0;
        margin_long_ = 100.0;
        process_orders_on_close_ = true;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        saw_clean_run_start = position_side_ == PositionSide::FLAT
            && !opening_affordability_pending_
            && !opening_affordability_eligible_
            && std::isnan(opening_affordability_raw_fill_base_);
        if (second_mode) {
            strategy_order("RAW", true, /*qty=*/10.0);
        } else {
            // 9*100 + 10% fee = 990 <= 1000: eligible but no trim.
            strategy_entry("L", true, kNaN, kNaN, /*qty=*/9.0);
        }
        process_pending_orders(current_bar_);
    }
};

static void test_run_reuse_clears_opening_state() {
    std::printf("test_run_reuse_clears_opening_state\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };

    ReuseOpeningProbe reused;
    reused.run(bars.data(), (int)bars.size());
    CHECK(!reused.opening_pending());
    CHECK(!reused.opening_eligible());
    CHECK(std::isnan(reused.opening_raw_base()));
    CHECK(near(reused.position_size(), 9.0));

    reused.second_mode = true;
    reused.run(bars.data(), (int)bars.size());
    CHECK(reused.saw_clean_run_start);
    CHECK(margin_call_rows(reused) == 1);
    CHECK(near(reused.position_size(), 6.0));

    ReuseOpeningProbe fresh;
    fresh.second_mode = true;
    fresh.run(bars.data(), (int)bars.size());
    CHECK(fresh.saw_clean_run_start);
    CHECK(fresh.trade_count() == reused.trade_count());
    CHECK(near(fresh.position_size(), reused.position_size()));
    CHECK(near(fresh.trade_size(0), reused.trade_size(0)));
    CHECK(near(fresh.entry_price(0), reused.entry_price(0)));
    CHECK(near(fresh.exit_price(0), reused.exit_price(0)));
}

// ---- D: leveraged long (2x) is liquidated by a falling market --------------

class LongLevLiqProbe : public MCEngine {
public:
    LongLevLiqProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 20.0;        // 20 @ 100 = 2000 notional = 2x equity
        commission_value_ = 0.0;
        margin_long_ = 50.0;              // 50% margin -> 2x limit; at the edge
        process_orders_on_close_ = true;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 20.0);
    }
};

static void test_long_leveraged_margin_call() {
    std::printf("test_long_leveraged_margin_call\n");
    // long 20 @ 100, equity 1000, margin 50% -> notional 2000 at the 2x limit.
    //   liqPrice = ((1000/20) - 100) / (0.5 - 1) = (50 - 100)/(-0.5) = 100.
    // A fall below 100 triggers a forced exit at the bar's LOW.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 101.0,  99.5, 100.0, 1.0),  // 0: long fills @100
        mk_bar(2000, 100.0, 100.0,  95.0,  96.0, 1.0),  // 1: low 95 < liq 100
        mk_bar(3000,  96.0,  97.0,  80.0,  82.0, 1.0),  // 2: deeper fall
    };
    LongLevLiqProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() >= 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    // First forced exit fills at bar1's adverse extreme (low = 95).
    CHECK(near(eng.exit_price(0), 95.0));
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 4.2105263157894735));
}

} // namespace

int main() {
    test_short_margin_call();
    test_short_margin_call_qty_step();
    test_margin_liquidation_price_formula();
    test_short_margin_call_disabled();
    test_long_100pct_margin_no_call();
    test_zero_cost_frozen_all_in_true_flat_gap_is_rejected();
    test_commissioned_frozen_all_in_true_flat_gap_is_eligible();
    test_paired_short_close_default_long_gap_remains_eligible();
    test_explicit_all_in_zero_comm_adverse_gap_declined();
    test_explicit_all_in_commissioned_adverse_gap_declined();
    test_explicit_all_in_zero_comm_no_qty_step_declined();
    test_explicit_all_in_fx_pointvalue_commission_declined();
    test_long_100pct_margin_trim_process_orders_on_close();
    test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip();
    test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip();
    test_raw_order_fresh_open_captures_affordability();
    test_accepted_add_fifo_keeps_add_affordability_event();
    test_rejected_add_preserves_opening_eligibility();
    test_zero_qty_add_preserves_opening_eligibility();
    test_zero_qty_add_does_not_duplicate_cash_per_order_fee();
    test_flat_clears_and_raw_fresh_reuses_state();
    test_reversal_captures_fresh_opening_state();
    test_run_reuse_clears_opening_state();
    test_long_leveraged_margin_call();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
