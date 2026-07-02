/*
 * test_margin_call.cpp — verify TradingView forced-liquidation (margin call).
 *
 * Covers the three behaviours of process_margin_call / margin_liquidation_price:
 *
 *   A. A 100%-equity SHORT held through an adverse (rising) move is force-
 *      liquidated. At least one "Margin call" exit is produced; the first one
 *      fills at the bar's adverse extreme (HIGH) and closes the documented 4x
 *      of the margin shortfall (capped at the full position). The reported
 *      margin_liquidation_price equals the closed-form formula while open.
 *
 *   B. A LONG at the default 100% margin can NEVER be liquidated (the formula
 *      denominator margin/100 - direction = 0). Even a catastrophic price
 *      crash produces NO margin call and margin_liquidation_price() == na.
 *
 *   C. A LEVERAGED long (margin_long = 20 => 5x) IS liquidated when price falls
 *      far enough; the forced exit fills at the bar's adverse extreme (LOW).
 *
 *   D. The margin-call emulator can be switched off (set_margin_call_enabled
 *      false); the underwater short is then held with no forced exit.
 *
 * Engine fill timing here uses process_orders_on_close = true so the market
 * entry fills at the signal bar's close, mirroring the p2 probe.
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
    double liq_price() const { return margin_liquidation_price(); }
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

    // Same scenario as test_short_margin_call. Unquantized, the first forced
    // lot is 4x the shortfall = 3.80952381 contracts. With a 0.5 lot step it
    // must floor DOWN to floor(3.80952381 / 0.5) * 0.5 = 3.5 (an exact step
    // multiple), and the exit price is unchanged (bar1 high = 105).
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
    // First quantized lot: floored to 3.5, an exact multiple of the 0.5 step.
    CHECK(near(eng.trade_size(0), 3.5));
    CHECK(is_multiple_of(eng.trade_size(0), step));
    // Quantization never enlarges the lot: floored <= unquantized 3.80952381.
    CHECK(eng.trade_size(0) <= 3.80952381 + 1e-9);
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(eng.exit_comment(0) == std::string("Margin call"));

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

// ---- B': OVER-ALLOCATED long at 100% margin IS force-liquidated -------------

class LongOverAllocProbe : public MCEngine {
public:
    LongOverAllocProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 10.0;
        commission_value_ = 0.0;
        margin_long_ = 100.0;             // 1x -> denominator (1 - 1) = 0 -> na
        process_orders_on_close_ = false;  // market entry fills at NEXT bar open
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, 10.0);
    }
};

static void test_long_100pct_margin_overalloc_call() {
    std::printf("test_long_100pct_margin_overalloc_call\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),  // 0: signal @ close 100
        mk_bar(2000, 110.0, 112.0, 108.0, 109.0, 1.0),  // 1: fills @ open 110
        mk_bar(3000, 109.0, 111.0, 107.0, 110.0, 1.0),  // 2: filler
    };
    LongOverAllocProbe eng;
    eng.run(bars.data(), (int)bars.size());
    // Signal-time affordability admits the fixed qty=10 against bar0's close
    // (10*100 == equity 1000), but the non-POOC market entry actually fills
    // at bar1's OPEN (110) -- notional 1100 > equity 1000, an over-allocated
    // 1x long. TradingView force-liquidates this on the SAME bar it fired.
    CHECK(eng.trade_count() >= 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.entry_price(0), 110.0));       // notional 1100 > equity 1000
    CHECK(near(eng.exit_price(0), 108.0));        // long adverse extreme = bar low
    CHECK(std::isnan(eng.liq_price()));           // still na for 1x long (matches TV)
}

// ---- C: leveraged long (5x) is liquidated by a falling market --------------

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
}

} // namespace

int main() {
    test_short_margin_call();
    test_short_margin_call_qty_step();
    test_margin_liquidation_price_formula();
    test_short_margin_call_disabled();
    test_long_100pct_margin_no_call();
    test_long_100pct_margin_overalloc_call();
    test_long_leveraged_margin_call();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
