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
    bool opening_default_short_reversal() const {
        return close_then_short_opening_requires_adverse_retry_;
    }
    double opening_raw_base() const {
        return opening_affordability_raw_fill_base_;
    }
    int live_entry_count() const { return position_entry_count_; }
};

// ---- A: 100%-equity short force-liquidated by a rising market --------------

class ShortLiqProbe : public MCEngine {
public:
    bool disable_mc_ = false;
    explicit ShortLiqProbe(bool disable_mc = false, double qty_step = 0.0,
                           double account_fx = 1.0) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;          // size the short at 100% of equity
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;               // 1x, default TV margin
        process_orders_on_close_ = true;     // market entry fills at bar close
        disable_mc_ = disable_mc;
        qty_step_ = qty_step;                // 0 = no lot quantization
        account_currency_fx_ = account_fx;
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
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
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

// A $100-scale all-in short can breach margin by less than one quantity step.
// TV still emits a Margin-call trade, but its truncated cover amount is zero;
// the broker closes the full residual instead of fabricating a one-step nibble.
class ShortZeroCoverProbe : public MCEngine {
public:
    explicit ShortZeroCoverProbe(double qty_step) {
        initial_capital_ = 100.384250;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = qty_step;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, kNaN);
        }
    }
};

static void test_short_margin_call_zero_cover_closes_full_residual() {
    std::printf("test_short_margin_call_zero_cover_closes_full_residual\n");
    std::vector<Bar> bars = {
        // Signal close freezes floor(100.38425 / 3788 / 0.0001) = 0.0265.
        mk_bar(1000, 3788.00, 3788.00, 3788.00, 3788.00, 1.0),
        // At HIGH: equity=100.37153, required=100.39472, so
        // q_min=0.000006121... < qty_step and the truncated cover is zero.
        mk_bar(2000, 3788.00, 3788.48, 3766.62, 3775.78, 1.0),
    };

    ShortZeroCoverProbe eng(/*qty_step=*/0.0001);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 3788.00));
    CHECK(near(eng.exit_price(0), 3788.48));
    CHECK(near(eng.trade_size(0), 0.0265));
    CHECK(near(eng.position_size(), 0.0));
}

static void test_short_margin_call_zero_cover_defaults_to_one_step() {
    std::printf("test_short_margin_call_zero_cover_defaults_to_one_step\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 3788.00, 3788.00, 3788.00, 3788.00, 1.0),
        mk_bar(2000, 3788.00, 3788.48, 3766.62, 3775.78, 1.0),
    };

    ShortZeroCoverProbe eng(/*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.0001));
    CHECK(near(eng.position_size(), -0.0264));
}

static void test_short_margin_call_exact_one_step_roundoff_keeps_four_x_nibble() {
    std::printf("test_short_margin_call_exact_one_step_roundoff_keeps_four_x_nibble\n");
    constexpr double step = 0.0001;
    // For a 10@100 all-in short, q_min = 20 - 2000/adverse. This adverse is
    // mathematically exactly one step, but the engine's equivalent arithmetic
    // represents the quotient just below 1. A bare floor would erase the lot
    // and incorrectly enter the zero-cover full-close fallback.
    const double adverse = 2000.0 / (20.0 - step);
    const double equity_at_high = 1000.0 - (adverse - 100.0) * 10.0;
    const double q_min = 10.0 - equity_at_high / adverse;
    const double step_count = q_min / step;
    CHECK(q_min < step);
    CHECK(std::abs(step_count - std::round(step_count)) < 1e-6);

    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 100.0, adverse, 99.0, 100.0, 1.0),
    };

    ShortLiqProbe default_eng(/*disable_mc=*/false, /*qty_step=*/step);
    default_eng.run(bars.data(), (int)bars.size());
    CHECK(default_eng.trade_count() == 1);
    CHECK(default_eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(default_eng.trade_size(0), step, 1e-12));
    CHECK(near(default_eng.position_size(), -(10.0 - step), 1e-12));

    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/step);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.exit_price(0), 100.01, 1e-12));
    CHECK(near(eng.trade_size(0), 4.0 * step, 1e-12));
    CHECK(near(eng.position_size(), -(10.0 - 4.0 * step), 1e-12));
}

static void test_short_margin_call_just_below_step_still_zero_covers() {
    std::printf("test_short_margin_call_just_below_step_still_zero_covers\n");
    constexpr double step = 0.0001;
    constexpr double below_guard_ratio = 1.0 - 2e-6;
    // This is genuinely below one step by more than the 1e-6 representation
    // guard. It must still quantize to zero and close the full residual.
    const double adverse = 2000.0 / (20.0 - step * below_guard_ratio);
    const double equity_at_high = 1000.0 - (adverse - 100.0) * 10.0;
    const double q_min = 10.0 - equity_at_high / adverse;
    const double step_count = q_min / step;
    CHECK(q_min < step);
    CHECK(std::abs(step_count - std::round(step_count)) > 1e-6);

    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),
        mk_bar(2000, 100.0, adverse, 99.0, 100.0, 1.0),
    };

    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/step);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.exit_price(0), 100.01, 1e-12));
    CHECK(near(eng.trade_size(0), 10.0, 1e-12));
    CHECK(near(eng.position_size(), 0.0, 1e-12));
}

static void test_short_margin_call_zero_cover_without_qty_step_stays_continuous() {
    std::printf("test_short_margin_call_zero_cover_without_qty_step_stays_continuous\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 3788.00, 3788.00, 3788.00, 3788.00, 1.0),
        mk_bar(2000, 3788.00, 3788.48, 3766.62, 3775.78, 1.0),
    };

    ShortZeroCoverProbe eng(/*qty_step=*/0.0);
    eng.run(bars.data(), (int)bars.size());

    const double opened_qty = 100.384250 / 3788.00;
    const double equity_at_high = 100.384250
        - (3788.48 - 3788.00) * opened_qty;
    const double q_min = opened_qty - equity_at_high / 3788.48;
    const double expected_liquidation = 4.0 * q_min;

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), expected_liquidation, 1e-9));
    CHECK(eng.trade_size(0) < opened_qty);
    CHECK(near(eng.position_size(), -(opened_qty - expected_liquidation), 1e-9));
}

static void test_short_margin_call_nonzero_cover_keeps_four_x_nibble() {
    std::printf("test_short_margin_call_nonzero_cover_keeps_four_x_nibble\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 99.0, 100.0, 1.0),
        // q_min=0.15085... = 1.50 qty steps. Floor-before-4x must
        // therefore close 0.4, not the full 10-contract position.
        mk_bar(2000, 100.0, 100.76, 99.0, 100.0, 1.0),
    };

    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/0.1);
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.exit_price(0), 100.76));
    CHECK(near(eng.trade_size(0), 0.4));
    CHECK(near(eng.position_size(), -9.6));
}

// Opening-affordability uses a separate, one-shot budget check. Its sub-lot
// shortfall is intentionally untradeable dust and must remain a no-op when the
// finite-price zero-cover fallback changes.
class ShortOpeningDustProbe : public MCEngine {
public:
    bool saw_actionable_opening_event = false;

    ShortOpeningDustProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.015;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 1.0;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/10.0);
        } else if (bar_index_ == 1) {
            // The next-open fill precedes this callback. Prove this fixture
            // actually reaches the opening-affordability branch before its
            // one-shot event is consumed at bar end.
            saw_actionable_opening_event = opening_affordability_pending_
                && opening_affordability_eligible_
                && std::isfinite(opening_affordability_raw_fill_base_);
        }
    }
};

static void test_short_opening_affordability_zero_cover_remains_dust_noop() {
    std::printf("test_short_opening_affordability_zero_cover_remains_dust_noop\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 99.99, 99.99, 99.99, 99.99, 1.0),
        // Required margin is 999.90. The 0.015% opening fee leaves equity
        // 999.850015, so q_min=0.0004999... < the 1-contract step.
        mk_bar(2000, 99.99, 99.99, 99.99, 99.99, 1.0),
    };

    ShortOpeningDustProbe eng;
    eng.set_syminfo_metadata("margin_zero_cover_full_liquidation", 1.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.saw_actionable_opening_event);
    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), -10.0));
    CHECK(!eng.opening_pending());
    CHECK(!eng.opening_eligible());
    CHECK(std::isnan(eng.opening_raw_base()));
}

static void test_short_margin_call_account_fx() {
    std::printf("test_short_margin_call_account_fx\n");
    constexpr double account_fx = 2.0;
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 101.0, 105.0, 100.5, 104.0, 1.0),
    };
    ShortLiqProbe eng(/*disable_mc=*/false, /*qty_step=*/0.0, account_fx);
    eng.run(bars.data(), (int)bars.size());

    // FX-aware percent sizing opens qty=1000/(100*2)=5. At high=105:
    // equity=1000-(105-100)*5*2=950, margin=5*105*2=1050, so the
    // finite-price 4x restore is 4*(5-950/(105*2)).
    const double expected_qty = 4.0 * (5.0 - 950.0 / (105.0 * account_fx));
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(near(eng.trade_size(0), expected_qty));
    CHECK(near(eng.position_size(), -(5.0 - expected_qty)));
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

// A commissioned, default-sized all-in long can remain margin-affordable
// before its opening fee while that fee alone creates a sub-step shortfall.
// TV's broker closes one whole contract for this exact true-flat MARKET shape
// when floor_step(q_min)==0.  These probes pin the rule, its full-position cap,
// the existing nonzero-floor 4x path, and two important scope exclusions.
class CommissionedDefaultPoeDustProbe : public MCEngine {
public:
    CommissionedDefaultPoeDustProbe(
            double initial_capital, double qty_step,
            CommissionType commission_type = CommissionType::PERCENT,
            double commission_value = 0.1,
            double default_percent = 100.0) {
        initial_capital_ = initial_capital;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = default_percent;
        commission_type_ = commission_type;
        commission_value_ = commission_value;
        margin_long_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.0001;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, kNaN);
        }
    }
};

static void test_fee_created_floor_zero_closes_one_contract() {
    std::printf("test_fee_created_floor_zero_closes_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1801.33, 1801.33, 1801.33, 1801.33, 1.0),
        mk_bar(2000, 1801.34, 1801.34, 1801.34, 1801.34, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/10000.0, /*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    // Frozen qty is 5.5459. Margin alone retains positive headroom, but the
    // 0.1% opening fee creates raw q_min=0.00002307... < one step.
    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 1801.34));
    CHECK(near(eng.exit_price(0), 1801.34));
    CHECK(near(eng.trade_size(0), 1.0));
    CHECK(near(eng.position_size(), 4.5459));
}

static void test_fee_created_floor_zero_caps_sub_one_position() {
    std::printf("test_fee_created_floor_zero_caps_sub_one_position\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1500.0, 1500.0, 1500.0, 1500.0, 1.0),
        mk_bar(2000, 1500.01, 1500.01, 1500.01, 1500.01, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/1000.0, /*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.666));
    CHECK(near(eng.position_size(), 0.0));
}

static void test_fee_created_sub_half_cent_deficit_respects_fx_ledger() {
    std::printf("test_fee_created_sub_half_cent_deficit_respects_fx_ledger\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1500.0, 1500.0, 1500.0, 1500.0, 1.0),
        mk_bar(2000, 1500.005, 1500.005, 1500.005, 1500.005, 1.0),
    };
    // q=0.666 leaves enough lot-floor headroom that the adverse fill creates
    // only a $0.00233 post-fee deficit. A same-currency broker compares the
    // raw amounts and applies the one-contract fallback (capped to the full
    // sub-one position).
    CommissionedDefaultPoeDustProbe same_currency(
        /*initial_capital=*/1000.0, /*qty_step=*/0.0001);
    same_currency.run(bars.data(), (int)bars.size());
    CHECK(same_currency.trade_count() == 1);
    CHECK(near(same_currency.trade_size(0), 0.666));
    CHECK(near(same_currency.position_size(), 0.0));

    // A configured quote->account provider selects TV's converted cent ledger.
    // Both bars use rate 1 so conversion lifecycle, not rate magnitude, is the
    // sole factor. The sub-half-cent remainder stays affordable.
    CommissionedDefaultPoeDustProbe converted_currency(
        /*initial_capital=*/1000.0, /*qty_step=*/0.0001);
    const int64_t timestamps[] = {0};
    const double rates[] = {1.0};
    CHECK(converted_currency.set_account_currency_fx_series(
        timestamps, rates, 1));
    converted_currency.run(bars.data(), (int)bars.size());
    CHECK(converted_currency.trade_count() == 0);
    CHECK(near(converted_currency.position_size(), 0.666));
}

static void test_fee_created_nonzero_floor_keeps_four_x_quantity() {
    std::printf("test_fee_created_nonzero_floor_keeps_four_x_quantity\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1872.19, 1872.19, 1872.19, 1872.19, 1.0),
        mk_bar(2000, 1872.27, 1872.27, 1872.27, 1872.27, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/9949.545946, /*qty_step=*/0.0001);
    eng.run(bars.data(), (int)bars.size());

    // raw q_min=0.00014707... floors to 0.0001 before the established 4x.
    CHECK(eng.trade_count() == 1);
    CHECK(near(eng.trade_size(0), 0.0004));
    CHECK(near(eng.position_size(), 5.3086));
}

static void test_fee_created_floor_zero_rejects_off_grid_one_contract() {
    std::printf("test_fee_created_floor_zero_rejects_off_grid_one_contract\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1800.0, 1800.0, 1800.0, 1800.0, 1.0),
        mk_bar(2000, 1800.01, 1800.01, 1800.01, 1800.01, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/9009.02, /*qty_step=*/2.5);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 5.0));
}

static void test_cash_per_order_floor_zero_stays_outside_percent_fee_rule() {
    std::printf("test_cash_per_order_floor_zero_stays_outside_percent_fee_rule\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1800.0, 1800.0, 1800.0, 1800.0, 1.0),
        mk_bar(2000, 1800.0, 1800.0, 1800.0, 1800.0, 1.0),
    };
    CommissionedDefaultPoeDustProbe eng(
        /*initial_capital=*/10000.0, /*qty_step=*/0.0001,
        CommissionType::CASH_PER_ORDER, /*commission_value=*/0.2);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 0);
    CHECK(near(eng.position_size(), 5.5555));
}

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

// Two explicit qty=2 market entries are each affordable on their own, but the
// accepted same-bar pyramid (qty=4) exceeds a 100%-margin account after the
// configured account-currency FX conversion and opening commissions. The
// resulting broker action is direction-symmetric: it restores margin from the
// raw matched fill, not from the short side's later adverse-price path.
class SameBarExplicitPairOpeningProbe : public MCEngine {
public:
    SameBarExplicitPairOpeningProbe(bool is_long, double account_fx,
                                    double initial_capital)
        : is_long_(is_long) {
        initial_capital_ = initial_capital;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        account_currency_fx_ = account_fx;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        strategy_entry("BASE", is_long_, kNaN, kNaN, /*qty=*/2.0);
        process_pending_orders(current_bar_);
        strategy_entry("ADD", is_long_, kNaN, kNaN, /*qty=*/2.0);
        process_pending_orders(current_bar_);
    }

private:
    bool is_long_;
};

static double expected_same_bar_pair_opening_liquidation(
        double initial_capital, double raw_fill, double account_fx) {
    constexpr double total_qty = 4.0;
    constexpr double cash_per_contract = 20.0;
    constexpr double qty_step = 0.0001;
    const double margin_per_unit = raw_fill * account_fx;
    const double opening_commission = total_qty * cash_per_contract;
    const double opening_equity = initial_capital - opening_commission;
    double q_min = total_qty - opening_equity / margin_per_unit;
    q_min = std::floor(q_min / qty_step) * qty_step;
    double qty_liq = 4.0 * q_min;
    qty_liq = std::floor(qty_liq / qty_step + 1e-6) * qty_step;
    return std::min(qty_liq, total_qty);
}

static void check_same_bar_explicit_pair_opening_trim(
        bool is_long, double account_fx, double initial_capital) {
    constexpr double raw_fill = 1741.23;
    constexpr double one_entry_qty = 2.0;
    constexpr double total_qty = 4.0;
    constexpr double entry_fee = 20.0;

    // The admission fork is cumulative, not per-order: each order fits, but
    // the accepted pair plus its account-native opening fees does not.
    CHECK(one_entry_qty * raw_fill * account_fx < initial_capital);
    CHECK(total_qty * raw_fill * account_fx + total_qty * entry_fee
          > initial_capital);

    SameBarExplicitPairOpeningProbe eng(is_long, account_fx, initial_capital);
    std::vector<Bar> bars = {
        mk_bar(1000, raw_fill, raw_fill, raw_fill, raw_fill, 1.0),
    };
    eng.run(bars.data(), (int)bars.size());

    const double expected_qty = expected_same_bar_pair_opening_liquidation(
        initial_capital, raw_fill, account_fx);
    double liquidated_qty = 0.0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        CHECK(eng.exit_comment(i) == std::string("Margin call"));
        CHECK(near(eng.entry_price(i), raw_fill));
        CHECK(near(eng.exit_price(i), raw_fill));
        liquidated_qty += eng.trade_size(i);
    }
    CHECK(margin_call_rows(eng) == 2);
    CHECK(near(liquidated_qty, expected_qty));
    CHECK(near(std::fabs(eng.position_size()), total_qty - expected_qty));
}

static void test_same_bar_explicit_pair_foreign_fx_direction_symmetry() {
    std::printf("test_same_bar_explicit_pair_foreign_fx_direction_symmetry\n");
    constexpr double account_fx = 88.0;
    constexpr double initial_capital = 500000.0;
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/true, account_fx, initial_capital);
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/false, account_fx, initial_capital);
}

static void test_same_bar_explicit_pair_fx1_direction_symmetry() {
    std::printf("test_same_bar_explicit_pair_fx1_direction_symmetry\n");
    constexpr double account_fx = 1.0;
    constexpr double initial_capital = 6000.0;
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/true, account_fx, initial_capital);
    check_same_bar_explicit_pair_opening_trim(
        /*is_long=*/false, account_fx, initial_capital);
}

// The add fills above the base short. Marking required margin at the VWAP
// (105) reports an exact 4*105 == 420 tie and incorrectly does nothing;
// TradingView marks the whole position at the latest raw fill (110), including
// the carried short's open loss, and immediately restores margin there.
class UnequalFillShortAddProbe : public MCEngine {
public:
    UnequalFillShortAddProbe() {
        initial_capital_ = 420.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("BASE", false, kNaN, kNaN, /*qty=*/2.0);
        } else if (bar_index_ == 1) {
            strategy_entry("ADD", false, kNaN, kNaN, /*qty=*/2.0);
        }
    }
};

static void test_short_add_opening_margin_marks_latest_raw_fill() {
    std::printf("test_short_add_opening_margin_marks_latest_raw_fill\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 110.0, 110.0, 110.0, 1.0),
    };
    UnequalFillShortAddProbe eng;
    eng.run(bars.data(), (int)bars.size());

    // At the latest fill, equity is 420 - (110-100)*2 = 400 and required
    // margin is 4*110=440. The 4x restore closes 4*(4-400/110).
    const double expected_qty = 4.0 * (4.0 - 400.0 / 110.0);
    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 100.0));
    CHECK(near(eng.exit_price(0), 110.0));
    CHECK(near(eng.trade_size(0), expected_qty));
    CHECK(near(eng.position_size(), -(4.0 - expected_qty)));
}

// Literal non-POOC geometry from the Thula margin fork. The effective fixed FX
// is deliberately inside the observed interval but remains an ordinary runtime
// input; the expected broker rows are pinned directly, not computed by a copy
// of the implementation formula.
class NextOpenExplicitShortPairProbe : public MCEngine {
public:
    NextOpenExplicitShortPairProbe() {
        // Prior realized loss leaves 497641.70 before these fills; four
        // account-native 20-per-contract opening fees make the broker's
        // opening-equity basis exactly 497561.70.
        initial_capital_ = 497641.70;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::CASH_PER_CONTRACT;
        commission_value_ = 20.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        account_currency_fx_ = 85.3567;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;
        strategy_entry("BASE", false, kNaN, kNaN, /*qty=*/2.0);
        strategy_entry("ADD", false, kNaN, kNaN, /*qty=*/2.0);
    }
};

static void test_thula_next_open_short_pair_exact_margin_rows() {
    std::printf("test_thula_next_open_short_pair_exact_margin_rows\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 1700.0, 1700.0, 1700.0, 1700.0, 1.0),
        mk_bar(2000, 1741.23, 1741.23, 1741.23, 1741.23, 1.0),
    };
    NextOpenExplicitShortPairProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 2);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 1741.23));
    CHECK(near(eng.entry_price(1), 1741.23));
    CHECK(near(eng.exit_price(0), 1741.23));
    CHECK(near(eng.exit_price(1), 1741.23));
    CHECK(near(eng.trade_size(0), 2.0));
    CHECK(near(eng.trade_size(1), 0.6088));
    CHECK(near(eng.position_size(), -1.3912));
}

class ShortOpeningEventScopeProbe : public MCEngine {
public:
    enum class Shape { DefaultPercent, DefaultCash, Priced, Raw, MarginNot100 };
    bool widened_event = false;
    bool priced_fill_observed = false;

    explicit ShortOpeningEventScopeProbe(Shape shape) : shape_(shape) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 2.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = shape == Shape::MarginNot100 ? 80.0 : 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
        if (shape == Shape::DefaultPercent) {
            default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
            default_qty_value_ = 50.0;
        } else if (shape == Shape::DefaultCash) {
            default_qty_type_ = QtyType::CASH;
            default_qty_value_ = 200.0;
        }
    }

    void on_bar(const Bar& /*bar*/) override {
        // The priced control must be a real fill, not merely a pending shape.
        // Arm it below the market on bar 0, then observe its short fill after
        // bar 1 gaps to the limit. dispatch_bar's step 1 applies that resting
        // order before this callback, while its event provenance is visible.
        if (shape_ == Shape::Priced) {
            if (bar_index_ == 0) {
                strategy_entry("S", false, /*limit=*/110.0, kNaN,
                               /*qty=*/2.0);
            } else if (bar_index_ == 1) {
                priced_fill_observed =
                    position_side_ == PositionSide::SHORT
                    && near(position_qty_, 2.0)
                    && !pyramid_entries_.empty()
                    && near(pyramid_entries_.back().price, 110.0);
                widened_event = opening_affordability_pending_
                    || opening_affordability_eligible_
                    || std::isfinite(opening_affordability_raw_fill_base_);
            }
            return;
        }

        if (bar_index_ != 0) return;
        switch (shape_) {
            case Shape::DefaultPercent:
            case Shape::DefaultCash:
                strategy_entry("S", false, kNaN, kNaN, kNaN);
                break;
            case Shape::Priced:
                break;  // handled above on two distinct bars
            case Shape::Raw:
                strategy_order("S", false, /*qty=*/2.0);
                break;
            case Shape::MarginNot100:
                strategy_entry("S", false, kNaN, kNaN, /*qty=*/2.0);
                break;
        }
        process_pending_orders(current_bar_);
        widened_event = opening_affordability_pending_
            || opening_affordability_eligible_
            || std::isfinite(opening_affordability_raw_fill_base_);
    }

private:
    Shape shape_;
};

static void test_short_opening_event_scope_is_explicit_market_margin100_only() {
    std::printf("test_short_opening_event_scope_is_explicit_market_margin100_only\n");
    const ShortOpeningEventScopeProbe::Shape shapes[] = {
        ShortOpeningEventScopeProbe::Shape::DefaultPercent,
        ShortOpeningEventScopeProbe::Shape::DefaultCash,
        ShortOpeningEventScopeProbe::Shape::Priced,
        ShortOpeningEventScopeProbe::Shape::Raw,
        ShortOpeningEventScopeProbe::Shape::MarginNot100,
    };
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 110.0, 110.0, 110.0, 110.0, 1.0),
    };
    for (auto shape : shapes) {
        ShortOpeningEventScopeProbe eng(shape);
        eng.run(bars.data(), (int)bars.size());
        if (shape == ShortOpeningEventScopeProbe::Shape::Priced) {
            CHECK(eng.priced_fill_observed);
        }
        CHECK(!eng.widened_event);
    }
}

// Literal first reversal from a source-bound TV tape. The script closes Long
// and then emits an omitted-qty Short in the same evaluation. Paid entry fees
// reduce broker equity, and the close-then-short fill receives both the
// fill-price affordability checkpoint and one bounded adverse-high retry.
class CommissionedDefaultShortCheckpointProbe : public MCEngine {
public:
    CommissionedDefaultShortCheckpointProbe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        calc_on_order_fills_ = false;
        bar_magnifier_enabled_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Long", true, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            strategy_close("Long");
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 2) {
            captured_short_event = opening_affordability_pending_
                && opening_affordability_eligible_
                && close_then_short_opening_requires_adverse_retry_
                && commissioned_all_in_market_short_lifecycle_
                && near(opening_affordability_raw_fill_base_, 1798.09);
        }
    }

    bool captured_short_event = false;
};

struct DefaultShortCheckpointResult {
    int margin_rows = 0;
    int trade_rows = 0;
    double position = 0.0;
    bool captured = false;
    std::vector<double> margin_qty;
    std::vector<double> margin_exit;
};

static DefaultShortCheckpointResult run_commissioned_default_short_checkpoint() {
    std::vector<Bar> bars = {
        mk_bar(1000, 1801.48, 1801.48, 1801.48, 1801.48, 1.0),
        mk_bar(2000, 1801.48, 1801.48, 1798.09, 1798.09, 1.0),
        mk_bar(3000, 1798.09, 1806.33, 1798.09, 1804.62, 1.0),
    };
    CommissionedDefaultShortCheckpointProbe eng;
    eng.run(bars.data(), static_cast<int>(bars.size()));

    DefaultShortCheckpointResult result;
    result.margin_rows = margin_call_rows(eng);
    result.trade_rows = eng.trade_count();
    result.position = eng.position_size();
    result.captured = eng.captured_short_event;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) != std::string("Margin call")) continue;
        result.margin_qty.push_back(eng.trade_size(i));
        result.margin_exit.push_back(eng.exit_price(i));
    }
    return result;
}

static void test_commissioned_close_then_short_exact_checkpoints() {
    std::printf(
        "test_commissioned_close_then_short_exact_checkpoints\n");
    const DefaultShortCheckpointResult result =
        run_commissioned_default_short_checkpoint();

    CHECK(result.captured);
    CHECK(result.margin_rows == 2);
    CHECK(result.margin_qty.size() == 2);
    CHECK(near(result.margin_qty[0], 0.0108, 1e-9));
    CHECK(near(result.margin_exit[0], 1798.09, 1e-9));
    CHECK(near(result.margin_qty[1], 0.1696, 1e-9));
    CHECK(near(result.margin_exit[1], 1806.33, 1e-9));
    CHECK(near(result.position, -5.3650, 1e-9));
}

// Mirror a separate close-then-entry order pair on the LONG side. A
// commissioned omitted-qty LONG created while SHORT after the close command
// retains the opening-affordability provenance needed by the broker trim.
class CommissionedCloseThenLongProbe : public MCEngine {
public:
    explicit CommissionedCloseThenLongProbe(bool explicit_qty)
        : explicit_qty_(explicit_qty) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            strategy_close("Short");
            const double qty = explicit_qty_
                ? frozen_default_market_qty(/*is_buy=*/true) : kNaN;
            strategy_entry("Long", true, kNaN, kNaN, qty);
        } else if (bar_index_ == 2) {
            captured = opening_affordability_pending_
                && opening_affordability_eligible_
                && commissioned_all_in_market_long_opening_affordability_
                && position_side_ == PositionSide::LONG;
        }
    }

    bool captured = false;

private:
    bool explicit_qty_;
};

static void test_commissioned_close_then_long_floor_zero_scope() {
    std::printf("test_commissioned_close_then_long_floor_zero_scope\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 2968.50, 2968.50, 2968.50, 2968.50, 1.0),
        mk_bar(2000, 2968.50, 2968.50, 2968.50, 2968.50, 1.0),
        mk_bar(3000, 2967.80, 2967.80, 2967.80, 2967.80, 1.0),
    };
    CommissionedCloseThenLongProbe omitted(/*explicit_qty=*/false);
    omitted.run(bars.data(), static_cast<int>(bars.size()));
    CommissionedCloseThenLongProbe explicit_control(/*explicit_qty=*/true);
    explicit_control.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(omitted.captured);
    CHECK(!explicit_control.captured);
    CHECK(omitted.trade_count() == 2);  // short close + long margin trim
    CHECK(margin_call_rows(omitted) == 1);
    CHECK(omitted.exit_comment(1) == std::string("Margin call"));
    CHECK(near(omitted.entry_price(1), 2967.80));
    CHECK(near(omitted.exit_price(1), 2967.80));
    CHECK(near(omitted.trade_size(1), 1.0, 1e-9));
    CHECK(explicit_control.trade_count() == 1);  // short close only
    CHECK(margin_call_rows(explicit_control) == 0);
}

// After the close-then-short fill-price trim, its bounded ordinary adverse
// retry can
// require a positive restore quantity smaller than one configured lot. TV's
// source-bound tape closes one whole contract at that exact discontinuity; the
// established generic finite-price behavior closes one qty_step instead.
class DefaultShortLaterFloorZeroProbe : public MCEngine {
public:
    explicit DefaultShortLaterFloorZeroProbe(bool full_residual = false) {
        initial_capital_ = 10000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata(
            "margin_zero_cover_full_liquidation",
            full_residual ? 1.0 : 0.0);

        constexpr double qty = 3.6930;
        constexpr double entry = 1799.94;
        constexpr double adverse = 1801.26;
        constexpr double raw_q_min = 0.00005;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        net_profit_sum_ =
            (qty - raw_q_min) * adverse - initial_capital_
            + (adverse - entry) * qty;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
    }

    void on_bar(const Bar&) override {}

    void trigger(bool carry_s_provenance) {
        current_bar_ = mk_bar(
            2000, 1800.00, 1801.26, 1799.50, 1800.50, 1.0);
        bar_index_ = 1;
        commissioned_all_in_market_short_lifecycle_ = carry_s_provenance;
        process_margin_call(current_bar_);
    }

    bool lifecycle_active() const {
        return commissioned_all_in_market_short_lifecycle_;
    }
};

static void test_default_short_lifecycle_floor_zero_one_contract() {
    std::printf("test_default_short_lifecycle_floor_zero_one_contract\n");
    DefaultShortLaterFloorZeroProbe top_level;
    top_level.trigger(/*carry_s_provenance=*/false);
    DefaultShortLaterFloorZeroProbe one_contract;
    one_contract.trigger(/*carry_s_provenance=*/true);
    DefaultShortLaterFloorZeroProbe full_residual(/*full_residual=*/true);
    full_residual.trigger(/*carry_s_provenance=*/true);

    // A normal short without default-opening lifecycle provenance retains the
    // established one-step progress fallback.
    CHECK(top_level.trade_count() == 1);
    CHECK(near(top_level.trade_size(0), 0.0001, 1e-9));
    CHECK(near(top_level.position_size(), -3.6929, 1e-9));

    CHECK(one_contract.trade_count() == 1);
    CHECK(one_contract.exit_comment(0) == std::string("Margin call"));
    CHECK(near(one_contract.exit_price(0), 1801.26));
    CHECK(near(one_contract.trade_size(0), 1.0, 1e-9));
    CHECK(near(one_contract.position_size(), -2.6930, 1e-9));
    CHECK(one_contract.lifecycle_active());

    // The existing opt-in whole-residual interpretation retains precedence
    // when a verifier deliberately combines both candidates.
    CHECK(full_residual.trade_count() == 1);
    CHECK(near(full_residual.trade_size(0), 3.6930, 1e-9));
    CHECK(near(full_residual.position_size(), 0.0, 1e-9));
}

// The commissioned lifecycle covers the opening checkpoint too. The positive
// restore quantity below is half one lot: without lifecycle provenance the
// event is a dust no-op; with it TV closes one contract at the raw fill base.
class DefaultShortOpeningFloorZeroProbe : public MCEngine {
public:
    explicit DefaultShortOpeningFloorZeroProbe(bool carry_lifecycle) {
        initial_capital_ = 1000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 10.0;
        constexpr double entry = 100.0;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        net_profit_sum_ = 0.495;  // entry fee 0.5 => q_min = 0.00005
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
        opening_affordability_pending_ = true;
        opening_affordability_eligible_ = true;
        close_then_short_opening_requires_adverse_retry_ = true;
        opening_affordability_raw_fill_base_ = entry;
        commissioned_all_in_market_short_lifecycle_ = carry_lifecycle;
    }

    void on_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }
};

static void test_default_short_opening_floor_zero_one_contract() {
    std::printf("test_default_short_opening_floor_zero_one_contract\n");
    DefaultShortOpeningFloorZeroProbe baseline(/*carry_lifecycle=*/false);
    baseline.trigger();
    DefaultShortOpeningFloorZeroProbe enabled(/*carry_lifecycle=*/true);
    enabled.trigger();

    // Without the scoped one-contract lifecycle, the opening checkpoint
    // dust-noops and its adverse retry follows the generic finite-price rule:
    // fee-net equity is 1000 + .495 - .5 = 999.995, so the positive 0.00005
    // restore amount floors below one 0.0001 lot and closes one qty_step.
    CHECK(baseline.trade_count() == 1);
    CHECK(baseline.exit_comment(0) == std::string("Margin call"));
    CHECK(near(baseline.trade_size(0), 0.0001, 1e-9));
    CHECK(near(baseline.position_size(), -9.9999, 1e-9));
    CHECK(enabled.trade_count() == 1);
    CHECK(enabled.exit_comment(0) == std::string("Margin call"));
    CHECK(near(enabled.entry_price(0), 100.0));
    CHECK(near(enabled.exit_price(0), 100.0));
    CHECK(near(enabled.trade_size(0), 1.0, 1e-9));
    CHECK(near(enabled.position_size(), -9.0, 1e-9));
}

// A close-then-short fill-price opening check can be affordable while the same
// bar's high is
// already adverse enough to require an ordinary margin call. The opening event
// must schedule that second checkpoint even though it emitted no trade, and it
// must be consumed before recursion so the retry occurs exactly once.
class DefaultShortAffordableOpeningAdverseProbe : public MCEngine {
public:
    DefaultShortAffordableOpeningAdverseProbe() {
        initial_capital_ = 1000.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 9.99;
        constexpr double entry = 100.0;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "S", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["S"] = qty;
        opening_affordability_pending_ = true;
        opening_affordability_eligible_ = true;
        close_then_short_opening_requires_adverse_retry_ = true;
        opening_affordability_raw_fill_base_ = entry;
        commissioned_all_in_market_short_lifecycle_ = true;
    }

    void on_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(2000, 100.0, 105.0, 99.0, 100.0, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
        event_cleared = !opening_affordability_pending_
            && !opening_affordability_eligible_
            && !close_then_short_opening_requires_adverse_retry_
            && std::isnan(opening_affordability_raw_fill_base_);
    }

    bool event_cleared = false;
};

static void test_default_short_affordable_opening_retries_adverse_once() {
    std::printf(
        "test_default_short_affordable_opening_retries_adverse_once\n");
    DefaultShortAffordableOpeningAdverseProbe probe;
    probe.trigger();

    CHECK(probe.trade_count() == 1);
    CHECK(probe.exit_comment(0) == std::string("Margin call"));
    CHECK(near(probe.entry_price(0), 100.0));
    CHECK(near(probe.exit_price(0), 105.0));
    // TV's adverse-margin ledger debits the surviving opening commission:
    // equity = 1000 + (100 - 105) * 9.99 - .4995 = 949.5505;
    // q_min = 9.99 - 949.5505 / 105 = 0.9466619..., which floors to
    // 0.9466 before TV's 4x liquidation multiplier.
    CHECK(near(probe.trade_size(0), 3.7864, 1e-9));
    CHECK(near(probe.position_size(), -6.2036, 1e-9));
    CHECK(probe.event_cleared);
}

// A default-sized commissioned short opened from true flat has its own
// lifecycle provenance. It must not inherit the prior close-then-short token,
// and an ordinary add or fresh position will clear it through the shared
// lifecycle reset sites.
class CommissionedDefaultFlatShortLifecycleProbe : public MCEngine {
public:
    explicit CommissionedDefaultFlatShortLifecycleProbe(bool commissioned) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = commissioned ? 0.05 : 0.0;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            captured = position_side_ == PositionSide::SHORT
                && commissioned_all_in_market_short_lifecycle_;
        }
    }

    bool captured = false;
};

static void test_commissioned_default_flat_short_lifecycle_tag() {
    std::printf("test_commissioned_default_flat_short_lifecycle_tag\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    CommissionedDefaultFlatShortLifecycleProbe uncommissioned(
        /*commissioned=*/false);
    uncommissioned.run(bars.data(), static_cast<int>(bars.size()));
    CommissionedDefaultFlatShortLifecycleProbe commissioned(
        /*commissioned=*/true);
    commissioned.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(!uncommissioned.captured);
    CHECK(commissioned.captured);
    CHECK(uncommissioned.trade_count() == 0);
    CHECK(commissioned.trade_count() == 0);
    CHECK(uncommissioned.position_size() < -1e-9);
    CHECK(commissioned.position_size() < -1e-9);
}

// After a prior LONG has already been fully liquidated, the next default SHORT
// is a true-flat open rather than a close-then-open reversal. Its adverse
// restore amount is positive but below one lot; TV caps the one-contract
// fallback to the entire 0.3383-contract residual.
class DefaultFlatShortFloorZeroProbe : public MCEngine {
public:
    explicit DefaultFlatShortFloorZeroProbe(bool carry_flat_lifecycle) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;

        constexpr double qty = 0.3383;
        constexpr double entry = 3734.88;
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = entry;
        position_entry_time_ = 1000;
        position_qty_ = qty;
        position_entry_count_ = 1;
        position_open_bar_ = 0;
        trail_best_price_ = entry;
        net_profit_sum_ = -8735.542085;
        pyramid_entries_.push_back(
            {entry, position_entry_time_, qty, "Short", 0});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["Short"] = qty;
        commissioned_all_in_market_short_lifecycle_ = carry_flat_lifecycle;
    }

    void on_bar(const Bar&) override {}

    void trigger() {
        current_bar_ = mk_bar(
            2000, 3734.88, 3735.52, 3734.00, 3735.00, 1.0);
        bar_index_ = 1;
        process_margin_call(current_bar_);
    }
};

static void test_default_flat_short_floor_zero_caps_to_residual() {
    std::printf("test_default_flat_short_floor_zero_caps_to_residual\n");
    DefaultFlatShortFloorZeroProbe baseline(
        /*carry_flat_lifecycle=*/false);
    baseline.trigger();
    DefaultFlatShortFloorZeroProbe enabled(
        /*carry_flat_lifecycle=*/true);
    enabled.trigger();

    CHECK(baseline.trade_count() == 1);
    CHECK(near(baseline.exit_price(0), 3735.52));
    CHECK(near(baseline.trade_size(0), 0.0001, 1e-9));
    CHECK(near(baseline.position_size(), -0.3382, 1e-9));

    CHECK(enabled.trade_count() == 1);
    CHECK(enabled.exit_comment(0) == std::string("Margin call"));
    CHECK(near(enabled.entry_price(0), 3734.88));
    CHECK(near(enabled.exit_price(0), 3735.52));
    CHECK(near(enabled.trade_size(0), 0.3383, 1e-9));
    CHECK(near(enabled.position_size(), 0.0, 1e-9));
}

// A commissioned omitted-qty 100%-equity SHORT acquires lifecycle provenance
// from its real entry shape. A user-requested partial exit changes that shape
// and must invalidate the one-contract floor-zero provenance. Broker margin
// reductions are covered separately above and intentionally preserve it.
class CommissionedDefaultShortPartialLifecycleProbe : public MCEngine {
public:
    CommissionedDefaultShortPartialLifecycleProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            lifecycle_after_open =
                commissioned_all_in_market_short_lifecycle_;
            strategy_close(
                "Short", "partial lifecycle close", /*qty=*/1.0,
                /*qty_percent=*/kNaN, /*immediately=*/true);
            qty_after_partial = position_qty_;
            lifecycle_after_partial =
                position_side_ == PositionSide::SHORT
                && position_qty_ > 1.0
                && commissioned_all_in_market_short_lifecycle_;
        }
    }

    void trigger_later_floor_zero() {
        constexpr double adverse = 105.0;
        constexpr double raw_q_min = 0.00005;
        const double open_fee = surviving_open_percent_commission_account();
        net_profit_sum_ =
            (position_qty_ - raw_q_min) * adverse - initial_capital_
            + open_fee
            + (adverse - position_entry_price_) * position_qty_;
        current_bar_ = mk_bar(
            3000, 100.0, adverse, 99.0, 100.0, 1.0);
        bar_index_ = 2;
        process_margin_call(current_bar_);
        lifecycle_after_margin_partial =
            commissioned_all_in_market_short_lifecycle_;
    }

    bool lifecycle_after_open = false;
    bool lifecycle_after_partial = false;
    bool lifecycle_after_margin_partial = false;
    double qty_after_partial = 0.0;
};

static void test_user_partial_invalidates_commissioned_short_lifecycle() {
    std::printf(
        "test_user_partial_invalidates_commissioned_short_lifecycle\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    CommissionedDefaultShortPartialLifecycleProbe probe;
    probe.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(probe.lifecycle_after_open);
    CHECK(!probe.lifecycle_after_partial);
    CHECK(probe.trade_count() == 1);
    CHECK(near(probe.trade_size(0), 1.0, 1e-9));
    const double qty_before_margin = probe.qty_after_partial;

    probe.trigger_later_floor_zero();
    CHECK(probe.trade_count() == 2);
    CHECK(probe.exit_comment(1) == std::string("Margin call"));
    CHECK(near(probe.exit_price(1), 105.0));
    CHECK(near(probe.trade_size(1), 0.0001, 1e-9));
    CHECK(near(probe.position_size(), -(qty_before_margin - 0.0001), 1e-9));
    CHECK(!probe.lifecycle_after_margin_partial);
}

// The scoped lifecycle belongs to one unmodified position cycle. A genuine
// accepted add invalidates it, while a full close clears it through the shared
// flat-position reset path.
class CommissionedDefaultShortLifecycleMutationProbe : public MCEngine {
public:
    enum class Mutation { AcceptedAdd, FullClose };

    explicit CommissionedDefaultShortLifecycleMutationProbe(Mutation mutation)
        : mutation_(mutation) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.05;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 2;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("Short", false, kNaN, kNaN, kNaN);
        } else if (bar_index_ == 1) {
            lifecycle_after_open =
                commissioned_all_in_market_short_lifecycle_;
            if (mutation_ == Mutation::AcceptedAdd) {
                strategy_entry("Add", false, kNaN, kNaN, /*qty=*/1.0);
            } else {
                strategy_close(
                    "Short", "full lifecycle close", /*qty=*/kNaN,
                    /*qty_percent=*/kNaN, /*immediately=*/true);
                full_close_cleared = position_side_ == PositionSide::FLAT
                    && !commissioned_all_in_market_short_lifecycle_;
            }
        } else if (bar_index_ == 2
                   && mutation_ == Mutation::AcceptedAdd) {
            add_filled = position_side_ == PositionSide::SHORT
                && position_entry_count_ == 2;
            accepted_add_cleared = add_filled
                && !commissioned_all_in_market_short_lifecycle_;
        }
    }

    bool lifecycle_after_open = false;
    bool add_filled = false;
    bool accepted_add_cleared = false;
    bool full_close_cleared = false;

private:
    Mutation mutation_;
};

static void test_commissioned_default_short_lifecycle_mutations() {
    std::printf("test_commissioned_default_short_lifecycle_mutations\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(2000, 100.0, 100.0, 100.0, 100.0, 1.0),
        mk_bar(3000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    CommissionedDefaultShortLifecycleMutationProbe add(
        CommissionedDefaultShortLifecycleMutationProbe::Mutation::AcceptedAdd);
    add.run(bars.data(), static_cast<int>(bars.size()));
    CommissionedDefaultShortLifecycleMutationProbe close(
        CommissionedDefaultShortLifecycleMutationProbe::Mutation::FullClose);
    close.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(add.lifecycle_after_open);
    CHECK(add.add_filled);
    CHECK(add.accepted_add_cleared);
    CHECK(close.lifecycle_after_open);
    CHECK(close.full_close_cleared);
}

// A scoped explicit MARKET short event is only provenance for that exact
// fill. If a later successful same-direction short fill in the same dispatch
// cycle has a non-scoped shape, the earlier event must not survive to the
// end-of-bar margin pass. These mutations use a later synthetic broker sample
// so BASE fills at 100 and the accepted add really fills at 110.
class ShortOpeningEventMutationProbe : public MCEngine {
public:
    enum class LaterFill { PricedEntry, RawOrder };
    bool base_event_captured = false;
    bool later_add_filled = false;
    bool stale_event_cleared = false;

    explicit ShortOpeningEventMutationProbe(LaterFill later_fill)
        : later_fill_(later_fill) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        pyramiding_ = 2;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        strategy_entry("BASE", false, kNaN, kNaN, /*qty=*/2.0);
        process_pending_orders(current_bar_);
        base_event_captured = opening_affordability_pending_
            && opening_affordability_eligible_
            && near(opening_affordability_raw_fill_base_, 100.0);

        if (later_fill_ == LaterFill::PricedEntry) {
            strategy_entry("ADD", false, /*limit=*/110.0, kNaN,
                           /*qty=*/2.0);
        } else {
            strategy_order("ADD", false, /*qty=*/2.0);
        }

        const Bar later_sample =
            mk_bar(current_bar_.timestamp, 110.0, 110.0, 110.0, 110.0, 1.0);
        process_pending_orders(later_sample);
        later_add_filled = position_side_ == PositionSide::SHORT
            && near(position_qty_, 4.0)
            && pyramid_entries_.size() == 2
            && near(pyramid_entries_.back().price, 110.0);
        stale_event_cleared = !opening_affordability_pending_
            && !opening_affordability_eligible_
            && std::isnan(opening_affordability_raw_fill_base_);
    }

private:
    LaterFill later_fill_;
};

static void test_priced_short_add_invalidates_scoped_opening_event() {
    std::printf("test_priced_short_add_invalidates_scoped_opening_event\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ShortOpeningEventMutationProbe eng(
        ShortOpeningEventMutationProbe::LaterFill::PricedEntry);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.base_event_captured);
    CHECK(eng.later_add_filled);
    CHECK(eng.stale_event_cleared);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.position_size(), -4.0));
}

static void test_raw_short_add_invalidates_scoped_opening_event() {
    std::printf("test_raw_short_add_invalidates_scoped_opening_event\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0, 100.0, 100.0, 1.0),
    };
    ShortOpeningEventMutationProbe eng(
        ShortOpeningEventMutationProbe::LaterFill::RawOrder);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.base_event_captured);
    CHECK(eng.later_add_filled);
    CHECK(eng.stale_event_cleared);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.position_size(), -4.0));
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

// A frozen 100%-equity MARKET reversal can pass the signal-time admission
// check yet become microscopically underfunded after the carried short is
// realized at the next-open fill. TV restores this positive sub-step deficit
// by closing exactly one whole contract, not by treating it as dust. The
// numbers pin a source-faithful omitted-quantity reversal event and also
// exercise the frozen-quantity no-refloor path: 5.2798 must survive placement
// and flip.
class DefaultLongReversalFloorZeroProbe : public MCEngine {
public:
    explicit DefaultLongReversalFloorZeroProbe(bool explicit_reversal)
        : explicit_reversal_(explicit_reversal) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        pyramiding_ = 1;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ != 0) return;

        // Seed the already-partially-liquidated short immediately before the
        // TV-pinned reversal signal. Its open mark at 1841.70 freezes the new
        // long at 5.2798; filling at 1841.71 realizes the remaining short loss
        // and leaves a positive restore amount below the 0.0001 lot step.
        position_side_ = PositionSide::SHORT;
        position_cycle_seq_ = next_position_cycle_seq_++;
        position_entry_price_ = 1821.96;
        position_entry_time_ = current_bar_.timestamp - 1000;
        position_qty_ = 5.2524;
        position_entry_count_ = 1;
        position_open_bar_ = -1;
        trail_best_price_ = position_entry_price_;
        net_profit_sum_ = -172.449012;
        pyramid_entries_.clear();
        id_unclosed_qty_.clear();
        pyramid_entries_.push_back(
            {position_entry_price_, position_entry_time_, position_qty_,
             "SEED", -1});
        pyramid_entries_.back().entry_incarnation = 1;
        snapshot_entry_commission(pyramid_entries_.back());
        id_unclosed_qty_["SEED"] = position_qty_;

        if (explicit_reversal_) {
            strategy_entry("L", true, kNaN, kNaN, 5.2798000001);
        } else {
            strategy_entry("L", true);
        }
    }

private:
    bool explicit_reversal_;
};

static std::vector<Bar> default_long_reversal_floor_zero_bars() {
    return {
        mk_bar(1000, 1841.70, 1841.70, 1841.70, 1841.70, 1.0),
        mk_bar(2000, 1841.71, 1841.71, 1841.71, 1841.71, 1.0),
    };
}

static void test_default_long_reversal_floor_zero_closes_one_contract() {
    std::printf("test_default_long_reversal_floor_zero_closes_one_contract\n");
    DefaultLongReversalFloorZeroProbe eng(/*explicit_reversal=*/false);
    auto bars = default_long_reversal_floor_zero_bars();
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.trade_count() == 2);  // seed close + same-fill long MC trim
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.entry_price(1), 1841.71));
    CHECK(near(eng.exit_price(1), 1841.71));
    CHECK(near(eng.trade_size(1), 1.0));
    CHECK(near(eng.position_size(), 4.2798));
}

static void test_explicit_long_reversal_floor_zero_remains_dust() {
    std::printf("test_explicit_long_reversal_floor_zero_remains_dust\n");
    DefaultLongReversalFloorZeroProbe eng(/*explicit_reversal=*/true);
    auto bars = default_long_reversal_floor_zero_bars();
    eng.run(bars.data(), static_cast<int>(bars.size()));

    CHECK(eng.trade_count() == 1);  // seed close only; no broker trim
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.position_size(), 5.2798));
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
    test_short_margin_call_zero_cover_closes_full_residual();
    test_short_margin_call_zero_cover_defaults_to_one_step();
    test_short_margin_call_exact_one_step_roundoff_keeps_four_x_nibble();
    test_short_margin_call_just_below_step_still_zero_covers();
    test_short_margin_call_zero_cover_without_qty_step_stays_continuous();
    test_short_margin_call_nonzero_cover_keeps_four_x_nibble();
    test_short_opening_affordability_zero_cover_remains_dust_noop();
    test_short_margin_call_account_fx();
    test_margin_liquidation_price_formula();
    test_short_margin_call_disabled();
    test_long_100pct_margin_no_call();
    test_zero_cost_frozen_all_in_true_flat_gap_is_rejected();
    test_commissioned_frozen_all_in_true_flat_gap_is_eligible();
    test_paired_short_close_default_long_gap_remains_eligible();
    test_fee_created_floor_zero_closes_one_contract();
    test_fee_created_floor_zero_caps_sub_one_position();
    test_fee_created_sub_half_cent_deficit_respects_fx_ledger();
    test_fee_created_nonzero_floor_keeps_four_x_quantity();
    test_fee_created_floor_zero_rejects_off_grid_one_contract();
    test_cash_per_order_floor_zero_stays_outside_percent_fee_rule();
    test_explicit_all_in_zero_comm_adverse_gap_declined();
    test_explicit_all_in_commissioned_adverse_gap_declined();
    test_explicit_all_in_zero_comm_no_qty_step_declined();
    test_explicit_all_in_fx_pointvalue_commission_declined();
    test_long_100pct_margin_trim_process_orders_on_close();
    test_long_100pct_margin_stop_trim_uses_raw_base_and_exit_slip();
    test_long_100pct_margin_limit_trim_uses_raw_base_and_exit_slip();
    test_raw_order_fresh_open_captures_affordability();
    test_same_bar_explicit_pair_foreign_fx_direction_symmetry();
    test_same_bar_explicit_pair_fx1_direction_symmetry();
    test_short_add_opening_margin_marks_latest_raw_fill();
    test_thula_next_open_short_pair_exact_margin_rows();
    test_short_opening_event_scope_is_explicit_market_margin100_only();
    test_commissioned_close_then_short_exact_checkpoints();
    test_commissioned_close_then_long_floor_zero_scope();
    test_default_short_lifecycle_floor_zero_one_contract();
    test_default_short_opening_floor_zero_one_contract();
    test_default_short_affordable_opening_retries_adverse_once();
    test_commissioned_default_flat_short_lifecycle_tag();
    test_default_flat_short_floor_zero_caps_to_residual();
    test_user_partial_invalidates_commissioned_short_lifecycle();
    test_commissioned_default_short_lifecycle_mutations();
    test_priced_short_add_invalidates_scoped_opening_event();
    test_raw_short_add_invalidates_scoped_opening_event();
    test_accepted_add_fifo_keeps_add_affordability_event();
    test_rejected_add_preserves_opening_eligibility();
    test_zero_qty_add_preserves_opening_eligibility();
    test_zero_qty_add_does_not_duplicate_cash_per_order_fee();
    test_flat_clears_and_raw_fresh_reuses_state();
    test_reversal_captures_fresh_opening_state();
    test_default_long_reversal_floor_zero_closes_one_contract();
    test_explicit_long_reversal_floor_zero_remains_dust();
    test_run_reuse_clears_opening_state();
    test_long_leveraged_margin_call();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
