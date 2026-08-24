/*
 * test_margin_call_gap_open.cpp — finding-430: forced liquidation at the
 * bar OPEN when a carried leveraged position already breaches the margin
 * requirement there (TradingView's broker emulator checks margin at every
 * point of the intrabar path, and the open is the first one).
 *
 *   A. Gap-open breach, no further breach at the extreme: exactly ONE
 *      "Margin call" row, filled AT THE OPEN, with the quantity computed at
 *      the open price (4x the open-priced shortfall).
 *   B. Gap-open breach AND a deeper breach at the adverse extreme: TWO
 *      "Margin call" rows on the same bar — the open slice first, then the
 *      survivor's extreme slice with the quantity computed at the extreme
 *      on the post-slice position.
 *   C. No open breach (open below the liquidation price, high above it):
 *      the established single adverse-extreme slice, bit-identical to the
 *      pre-fix engine (regression guard for on-tick feeds without gaps).
 *   D. Whole-share lot grid (qty_step = 1, the NASDAQ:AAPL tape shape):
 *      floor-before-4x at the open price and the one-contract fallback when
 *      the open-priced shortfall floors to zero.
 *   E. A leveraged LONG gapping DOWN through its liquidation price is
 *      sliced at the open on the same terms.
 *   F. The emulator switch (set_margin_call_enabled(false)) disables the
 *      open slice together with the rest of the forced-liquidation family.
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

class MCEngine : public BacktestEngine {
public:
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
};

// 100%-equity short at 1x margin (TV default margin_short=100), market entry
// filling at bar0 close = 100 -> qty 10, liquidation price 100.
class ShortProbe : public MCEngine {
public:
    explicit ShortProbe(double qty_step = 0.0, bool disable_mc = false) {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = true;
        qty_step_ = qty_step;
        syminfo_mintick_ = 0.01;
        if (disable_mc) set_margin_call_enabled(false);
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("S", false, kNaN, kNaN, kNaN);
    }
};

// 5x leveraged long (margin_long = 20), 100% of equity: qty 10 @ 100,
// liquidation price = (100 - 100) / (0.2 - 1) ... = 100 - 100/(10*... ) see
// compute_liquidation_price: (equity/(qty*pv) - entry) / (m - 1)
//   = (1000/10 - 100) / (0.2 - 1) = 0 / -0.8 = 100 -> wait: at 100% of equity
// the long's margin requirement 10*100*0.2 = 200 <= 1000, so liq is where
// equity(P) = 0.2*10*P: 1000 + (P-100)*10 = 2P -> 8P = 0 ... use the engine's
// formula directly in the assertions below instead of restating it.
class LevLongProbe : public MCEngine {
public:
    LevLongProbe() {
        initial_capital_ = 1000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 20.0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("L", true, kNaN, kNaN, kNaN);
    }
};

static int count_margin_calls(const MCEngine& e) {
    int n = 0;
    for (int i = 0; i < e.trade_count(); ++i)
        if (e.exit_comment(i) == std::string("Margin call")) ++n;
    return n;
}

// ---- A: gap-open breach, extreme does not breach the survivor -------------
static void test_gap_open_single_slice_at_open() {
    std::printf("test_gap_open_single_slice_at_open\n");
    // bar1 opens at 104 (> liq 100). At the open: equity = 1000 - 4*10 = 960,
    // required = 10*104 = 1040 -> q_min = 10 - 960/104 = 0.769231, 4x = 3.076923.
    // Survivor 6.923077 @ high 106: equity = 1000 - 4*3.076923 - 6*6.923077
    // = 945.85, required = 6.923077*106 = 733.85 -> no second slice.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 104.0, 106.0, 103.0, 105.0, 1.0),
        mk_bar(3000, 105.0, 105.5, 104.0, 105.0, 1.0),
    };
    ShortProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(count_margin_calls(eng) == 1);
    CHECK(eng.trade_count() >= 1);
    if (eng.trade_count() >= 1) {
        CHECK(eng.exit_comment(0) == std::string("Margin call"));
        CHECK(eng.exit_bar(0) == 1);
        CHECK(near(eng.exit_price(0), 104.0));          // AT THE OPEN, not the high
        CHECK(near(eng.trade_size(0), 3.0769230769, 1e-6));  // open-priced 4x shortfall
    }
    CHECK(near(eng.position_size(), -(10.0 - 3.0769230769), 1e-6));
}

// ---- B: gap-open breach + deeper extreme breach: two slices on one bar ----
static void test_gap_open_then_extreme_second_slice() {
    std::printf("test_gap_open_then_extreme_second_slice\n");
    // bar1: open 104 -> open slice 3.076923 (as above), survivor 6.923077.
    // high 130: equity = 1000 - 12.307692 - 30*6.923077 = 780.0,
    // required = 6.923077*130 = 900.0 -> q_min = 6.923077 - 780/130 = 0.923077,
    // 4x = 3.692308 -> second "Margin call" row @130 on the same bar.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 104.0, 130.0, 103.0, 128.0, 1.0),
        mk_bar(3000, 128.0, 128.5, 127.0, 128.0, 1.0),
    };
    ShortProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() >= 2);
    if (eng.trade_count() >= 2) {
        CHECK(eng.exit_comment(0) == std::string("Margin call"));
        CHECK(eng.exit_comment(1) == std::string("Margin call"));
        CHECK(eng.exit_bar(0) == 1);
        CHECK(eng.exit_bar(1) == 1);
        CHECK(near(eng.exit_price(0), 104.0));
        CHECK(near(eng.trade_size(0), 3.0769230769, 1e-6));
        CHECK(near(eng.exit_price(1), 130.0));
        CHECK(near(eng.trade_size(1), 3.6923076923, 1e-6));
    }
}

// ---- C: no open breach -> the established single extreme slice -----------
static void test_no_open_breach_keeps_extreme_only() {
    std::printf("test_no_open_breach_keeps_extreme_only\n");
    // bar1 opens at 99.5 (< liq 100): no open slice. high 105 -> the ordinary
    // extreme slice: equity@105 = 950, required 1050, q_min 0.952381, 4x
    // 3.809524 @105 (the test_margin_call.cpp reference values).
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000,  99.5, 105.0,  99.0, 104.0, 1.0),
        mk_bar(3000, 104.0, 104.5, 103.0, 104.0, 1.0),
    };
    ShortProbe eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(count_margin_calls(eng) == 1);
    if (eng.trade_count() >= 1) {
        CHECK(near(eng.exit_price(0), 105.0));
        CHECK(near(eng.trade_size(0), 3.80952381, 1e-4));
        CHECK(eng.exit_bar(0) == 1);
    }
}

// ---- D: whole-share lot grid ---------------------------------------------
static void test_gap_open_whole_share_grid() {
    std::printf("test_gap_open_whole_share_grid\n");
    // qty_step = 1: bar0 short 10 @100. bar1 open 104: raw q_min 0.769231
    // floors to 0 -> the one-contract fallback closes exactly 1 share @104.
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
            mk_bar(2000, 104.0, 104.5, 103.0, 104.0, 1.0),
            mk_bar(3000, 104.0, 104.5, 103.0, 104.0, 1.0),
        };
        ShortProbe eng(/*qty_step=*/1.0);
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trade_count() >= 1);
        if (eng.trade_count() >= 1) {
            CHECK(eng.exit_comment(0) == std::string("Margin call"));
            CHECK(near(eng.exit_price(0), 104.0));
            CHECK(near(eng.trade_size(0), 1.0));
            CHECK(eng.exit_bar(0) == 1);
        }
    }
    // Larger gap: open 112 -> equity 880, required 1120, q_min = 10 - 880/112
    // = 2.142857 -> floor 2 -> 4x = 8 shares @112 (floor-before-4x at the
    // OPEN price; the high-priced rule would give a different lot).
    {
        std::vector<Bar> bars = {
            mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
            mk_bar(2000, 112.0, 112.5, 111.0, 112.0, 1.0),
            mk_bar(3000, 112.0, 112.5, 111.0, 112.0, 1.0),
        };
        ShortProbe eng(/*qty_step=*/1.0);
        eng.run(bars.data(), (int)bars.size());
        CHECK(eng.trade_count() >= 1);
        if (eng.trade_count() >= 1) {
            CHECK(eng.exit_comment(0) == std::string("Margin call"));
            CHECK(near(eng.exit_price(0), 112.0));
            CHECK(near(eng.trade_size(0), 8.0));
        }
        // Survivor 2 @112.5: equity = 1000 - 12*8 - 12.5*2 = 879, required
        // 225 -> no second slice on this bar.
        CHECK(count_margin_calls(eng) == 1);
        CHECK(near(eng.position_size(), -2.0));
    }
}

// ---- E: leveraged long gapping down ---------------------------------------
static void test_leveraged_long_gap_down() {
    std::printf("test_leveraged_long_gap_down\n");
    // 5x long 10 @100 (required margin 200 of equity 1000). Liquidation where
    // 1000 + (P-100)*10 = 0.2*10*P -> 8P = 0 ... i.e. P = 0? No: equity(P) =
    // 1000 + 10*(P-100) = 10P; required = 2P; 10P >= 2P always -> at 100% of
    // equity a 5x long is never in deficit. Use an 400%-of-equity long instead:
    // qty 40 @100 (required 800 <= 1000). equity(P) = 1000 + 40*(P-100)
    // = 40P - 3000; required = 8P -> deficit when 32P < 3000 -> P < 93.75.
    class BigLevLong : public LevLongProbe {
    public:
        BigLevLong() { default_qty_value_ = 400.0; }
    };
    // bar1 gaps down to 90: equity = 40*90 - 3000 = 600, required = 720,
    // q_min = 40 - 600/18 = 6.666667, 4x = 26.666667 @90 at the open.
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000,  90.0,  91.0,  89.5,  90.5, 1.0),
        mk_bar(3000,  90.5,  91.0,  90.0,  90.5, 1.0),
    };
    BigLevLong eng;
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() >= 1);
    if (eng.trade_count() >= 1) {
        CHECK(eng.exit_comment(0) == std::string("Margin call"));
        CHECK(eng.exit_bar(0) == 1);
        CHECK(near(eng.exit_price(0), 90.0));
        CHECK(near(eng.trade_size(0), 26.6666666667, 1e-6));
    }
}

// ---- F: emulator switch ---------------------------------------------------
static void test_gap_open_disabled() {
    std::printf("test_gap_open_disabled\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 100.0, 100.0,  99.0, 100.0, 1.0),
        mk_bar(2000, 104.0, 130.0, 103.0, 128.0, 1.0),
        mk_bar(3000, 128.0, 128.5, 127.0, 128.0, 1.0),
    };
    ShortProbe eng(/*qty_step=*/0.0, /*disable_mc=*/true);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 0);
}

}  // namespace

int main() {
    test_gap_open_single_slice_at_open();
    test_gap_open_then_extreme_second_slice();
    test_no_open_breach_keeps_extreme_only();
    test_gap_open_whole_share_grid();
    test_leveraged_long_gap_down();
    test_gap_open_disabled();
    std::printf("%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
