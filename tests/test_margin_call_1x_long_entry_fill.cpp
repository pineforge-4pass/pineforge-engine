/*
 * test_margin_call_1x_long_entry_fill.cpp — finding-325: 1x-long entry-fill
 * affordability chronology.
 *
 * TV evaluates the 1x-long (margin_long=100) opening-affordability check AT
 * THE ENTRY FILL, chronologically before the same bar's intrabar exits. The
 * #149 hook (margin_call_slice_before_priced_exit) deliberately excluded 1x
 * longs — compute_liquidation_price() is na there — so a same-bar full exit
 * hid the deficit (the end-of-bar event found the position already gone) and
 * the engine filled the exit on the FULL position. The rhyme17 exemplar
 * (2026-01-09 14:30, long reversal 3.5168 @3094.06, sub-lot deficit 0.0302
 * USD): TV books a 1.0-contract pnl-0 "Margin call" row at the raw entry
 * fill price FIRST, then the stop closes only the 2.5168 remainder.
 *
 * The deficit class this reproduces: an omitted-qty percent-of-equity=100
 * reversal is frozen against the SIGNAL close C (lot-floored, leaving a
 * sub-lot budget remainder r), then fills at a one-mintick UPTICK O=C+tick.
 * KI-54 admits while the uptick notional on the frozen lot stays inside r,
 * but the opening cost q*O against POST-CLOSE equity overshoots by
 * tick*(q+s) - r — a positive, sub-lot deficit whose restore quantity floors
 * to zero and takes the one-contract fallback. A commissioned explicit-qty
 * open reaches the same discontinuity through its entry fee instead.
 *
 * Reversal scaffold (percent=100, commission 0, qty_step 0.0001): SHORT
 * 3.3333 @3000; signal close 2997.49 -> eq 10008.3666, frozen long qty
 * 3.3389 (remainder r=0.0472); fill @2997.50 -> admit (0.0334 <= r), realized
 * eq 10008.3333, cost 10008.3527 -> deficit 0.0195 -> one-contract fallback.
 * Commissioned scaffold (explicit qty 99.95 @100, 0.1% fee): opening budget
 * 9990.005 < 9995 -> raw restore 0.04995 floors to 0.0499 -> 4x = 0.1996.
 *
 *   A. Reversal + same-bar stop: slice 1.0 @2997.50 (pnl 0, "Margin call")
 *      BEFORE the stop, which closes the 2.3389 remainder. (The exemplar
 *      shape; RED pre-fix: the stop fills the full 3.3389.)
 *   B. Commissioned long + same-bar stop: ordinary floor-before-4x nibble
 *      0.1996 @100 first, stop closes 99.7504. (RED pre-fix.)
 *   C. Zero-tick reversal fill (O == C): no deficit -> no Margin-call row.
 *   D. Reversal, stop never touched -> the event keeps its established
 *      END-OF-BAR placement (identical rows pre/post fix), survivor held.
 *   E. Commissioned SHORT mirror is untouched (LONG-only extension): the
 *      stop still closes the full position, no Margin-call row.
 *   F. POOC: the opening check keeps its end-of-bar placement.
 *   G. Emulator off -> nothing fires (full stop close).
 *   H. Handle reuse: a rerun reproduces the same rows.
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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

namespace {

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk_bar(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

class MCEngine : public BacktestEngine {
public:
    std::string exit_comment(int i) const { return closed_trade_exit_comment(i); }
    std::string exit_id(int i) const { return closed_trade_exit_id(i); }
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    double trade_size(int i) const { return closed_trade_size(i); }
    double trade_pnl(int i) const { return closed_trade_profit(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
};

static int margin_call_rows(const MCEngine& eng) {
    int count = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) == std::string("Margin call")) ++count;
    }
    return count;
}

// The exemplar shape: default-sized (percent=100) SHORT opened @3000, then a
// default-sized LONG reversal signalled on bar2 (close C) together with its
// stop bracket, filling at bar3's open. Commission 0, qty_step 0.0001.
class ReversalProbe : public MCEngine {
public:
    explicit ReversalProbe(double stop_level, bool disable_mc = false)
        : stop_level_(stop_level) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = false;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        if (disable_mc) set_margin_call_enabled(false);
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) strategy_entry("S", false);
        if (bar_index_ == 2) {
            strategy_entry("L", true);
            strategy_exit("X", "L", kNaN, stop_level_, kNaN, kNaN, kNaN,
                          100.0, "");
        }
    }

private:
    double stop_level_;
};

// bar3 opens one mintick ABOVE the bar2 signal close 2997.49 -> the frozen
// 3.3389 long admits inside the lot-floor remainder but overshoots the
// post-close equity by 0.0195 (sub-lot -> one-contract fallback).
static std::vector<Bar> reversal_bars(double o3, double l3, double c3) {
    return {
        mk_bar(1000, 3000, 3000, 3000, 3000),        // 0: short signal
        mk_bar(2000, 3000, 3000, 2995, 3000),        // 1: S fills @3000
        mk_bar(3000, 3000, 3000, 2996, 2997.49),     // 2: reversal signal
        mk_bar(4000, o3, o3, l3, c3),                // 3: fill + same-bar stop
        mk_bar(5000, c3, c3, c3, c3),                // 4
    };
}

// Commissioned explicit-qty scaffold: MARKET qty 99.95 @100 with a 0.1% fee
// (opening budget 9990.005 < notional 9995 -> restore 0.04995 -> 4x 0.1996).
class CommissionedProbe : public MCEngine {
public:
    CommissionedProbe(bool is_long, double stop_level, bool pooc = false)
        : is_long_(is_long), stop_level_(stop_level) {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::FIXED;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.1;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        process_orders_on_close_ = pooc;
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
    }

    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0) {
            strategy_entry("L", is_long_, kNaN, kNaN, /*qty=*/99.95);
            strategy_exit("X", "L", kNaN, stop_level_, kNaN, kNaN, kNaN,
                          100.0, "");
        }
    }

private:
    bool is_long_;
    double stop_level_;
};

}  // namespace

// ---- A: the exemplar — sub-lot deficit, one-contract fallback FIRST --------

static void test_one_contract_slice_before_same_bar_stop() {
    std::printf("test_one_contract_slice_before_same_bar_stop\n");
    ReversalProbe eng(/*stop=*/2967.51);
    auto bars = reversal_bars(2997.50, 2960.0, 2965.0);
    eng.run(bars.data(), (int)bars.size());

    // Row order is the TV chronology: the short's reversal close, the pnl-0
    // one-contract "Margin call" trim at the RAW entry fill base, then the
    // stop closing the reduced remainder.
    CHECK(eng.trade_count() == 3);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(near(eng.trade_size(0), 3.3333));
    CHECK(near(eng.exit_price(0), 2997.50));
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.trade_size(1), 1.0));
    CHECK(near(eng.entry_price(1), 2997.50));
    CHECK(near(eng.exit_price(1), 2997.50));       // RAW entry fill base
    CHECK(near(eng.trade_pnl(1), 0.0));            // the TV pnl-0 row
    CHECK(eng.exit_bar(1) == 3);
    CHECK(eng.exit_comment(2) != std::string("Margin call"));
    CHECK(eng.exit_id(2) == std::string("X"));
    CHECK(near(eng.trade_size(2), 2.3389));        // the reduced remainder
    CHECK(near(eng.exit_price(2), 2967.51));
    CHECK(eng.exit_bar(2) == 3);
    CHECK(near(eng.position_size(), 0.0));
}

// ---- B: a lot-expressible deficit uses the ordinary floor-before-4x --------

static void test_four_x_nibble_slice_before_same_bar_stop() {
    std::printf("test_four_x_nibble_slice_before_same_bar_stop\n");
    CommissionedProbe eng(/*is_long=*/true, /*stop=*/95.0);
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),          // 0: signal
        mk_bar(2000, 100, 100, 94, 94),            // 1: fill + same-bar stop
        mk_bar(3000, 94, 94, 94, 94),              // 2
    };
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.1996));
    CHECK(near(eng.exit_price(0), 100.0));
    CHECK(eng.exit_bar(0) == 1);
    CHECK(eng.exit_id(1) == std::string("X"));
    CHECK(near(eng.trade_size(1), 99.7504));
    CHECK(near(eng.exit_price(1), 95.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- C: a zero-tick reversal fill has no deficit -> quiet ------------------

static void test_zero_tick_fill_stays_quiet() {
    std::printf("test_zero_tick_fill_stays_quiet\n");
    ReversalProbe eng(/*stop=*/2967.51);
    auto bars = reversal_bars(2997.49, 2960.0, 2965.0);   // O == C
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(1), 3.3389));        // full position
    CHECK(near(eng.exit_price(1), 2967.51));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- D: no same-bar priced exit -> end-of-bar placement preserved ----------

static void test_no_same_bar_exit_keeps_end_of_bar_event() {
    std::printf("test_no_same_bar_exit_keeps_end_of_bar_event\n");
    // Stop far below the bar: the one-shot event books its trim at the
    // established end-of-bar point exactly as before the fix.
    ReversalProbe eng(/*stop=*/2000.0);
    auto bars = reversal_bars(2997.50, 2990.0, 2995.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(1) == std::string("Margin call"));
    CHECK(near(eng.trade_size(1), 1.0));
    CHECK(near(eng.exit_price(1), 2997.50));
    CHECK(eng.exit_bar(1) == 3);
    CHECK(near(eng.position_size(), 2.3389));      // survivor held
}

// ---- E: the commissioned SHORT mirror is untouched (LONG-only) -------------

static void test_short_one_x_mirror_untouched() {
    std::printf("test_short_one_x_mirror_untouched\n");
    // Same fee-created opening deficit on the short side; the stop above the
    // open still fills the FULL position first (the established behavior on
    // the short side) and no Margin-call row appears.
    CommissionedProbe eng(/*is_long=*/false, /*stop=*/105.0);
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),          // 0: signal
        mk_bar(2000, 100, 106, 100, 105),          // 1: fill + same-bar stop
        mk_bar(3000, 105, 105, 105, 105),          // 2
    };
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 99.95));
    CHECK(near(eng.exit_price(0), 105.0));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- F: POOC keeps the end-of-bar placement --------------------------------

static void test_pooc_keeps_end_of_bar_event() {
    std::printf("test_pooc_keeps_end_of_bar_event\n");
    // Under process_orders_on_close the entry fills at the bar-0 close; the
    // opening check still runs end-of-bar (unchanged) and trims 0.1996 @100.
    // The far stop never fills, pinning only the event placement.
    CommissionedProbe eng(/*is_long=*/true, /*stop=*/80.0, /*pooc=*/true);
    std::vector<Bar> bars = {
        mk_bar(1000, 100, 100, 100, 100),          // 0: signal + close fill
        mk_bar(2000, 100, 100, 100, 100),          // 1
    };
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.1996));
    CHECK(near(eng.exit_price(0), 100.0));
    CHECK(eng.exit_bar(0) == 0);                   // end-of-bar on the fill bar
    CHECK(near(eng.position_size(), 99.7504));
}

// ---- G: emulator off -> nothing fires --------------------------------------

static void test_disabled_emulator_stays_quiet() {
    std::printf("test_disabled_emulator_stays_quiet\n");
    ReversalProbe eng(/*stop=*/2967.51, /*disable_mc=*/true);
    auto bars = reversal_bars(2997.50, 2960.0, 2965.0);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(1), 3.3389));        // full stop close
    CHECK(near(eng.exit_price(1), 2967.51));
}

// ---- H: handle reuse reproduces the same rows ------------------------------

static void test_rerun_reproduces_slice() {
    std::printf("test_rerun_reproduces_slice\n");
    ReversalProbe eng(/*stop=*/2967.51);
    auto bars = reversal_bars(2997.50, 2960.0, 2965.0);
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 3);
    CHECK(margin_call_rows(eng) == 1);

    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.trade_count() == 3);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(near(eng.trade_size(1), 1.0));
    CHECK(near(eng.exit_price(1), 2997.50));
    CHECK(near(eng.trade_size(2), 2.3389));
    CHECK(near(eng.exit_price(2), 2967.51));
}

int main() {
    std::printf("=== test_margin_call_1x_long_entry_fill ===\n");

    test_one_contract_slice_before_same_bar_stop();
    test_four_x_nibble_slice_before_same_bar_stop();
    test_zero_tick_fill_stays_quiet();
    test_no_same_bar_exit_keeps_end_of_bar_event();
    test_short_one_x_mirror_untouched();
    test_pooc_keeps_end_of_bar_event();
    test_disabled_emulator_stays_quiet();
    test_rerun_reproduces_slice();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
