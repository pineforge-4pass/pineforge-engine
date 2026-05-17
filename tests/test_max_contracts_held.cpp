/*
 * test_max_contracts_held.cpp — verify BacktestEngine tracks
 * strategy.max_contracts_held_all / _long / _short correctly.
 *
 * Engine order-fill semantics (process_orders_on_close = false, default):
 *   Orders placed in on_bar(N) fill at the START of on_bar(N+1).
 *   Therefore entry placed on bar 0 fills on bar 1; close on bar 1 fills on bar 2.
 *
 * Scenario (3 trades, 12 bars):
 *   Bar 0: place LONG entry qty=3
 *   Bar 1: fills long 3 @ bar1.close=100; hold
 *   Bar 2: place strategy_close — fills on bar 3
 *   Bar 3: close long @ 100 (even trade: entry=100, exit=100, no commission)
 *   Bar 4: place SHORT entry qty=5
 *   Bar 5: fills short 5 @ bar5.close=200; hold
 *   Bar 6: place strategy_close — fills on bar 7
 *   Bar 7: cover short @ 150 (profit)
 *   Bar 8: place LONG entry qty=2
 *   Bar 9: fills long 2 @ bar9.close=180; hold
 *   Bar 10: place strategy_close — fills on bar 11
 *   Bar 11: exit long @ 170 (loss)
 *
 * After all bars:
 *   max_contracts_held_all   = 5   (bars 5-7, short leg)
 *   max_contracts_held_long  = 3   (bars 1-3, first long leg)
 *   max_contracts_held_short = 5   (bars 5-7, short leg)
 *   eventrades               = 1   (bar 3: entry=100 exit=100, zero commission)
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

static Bar mk_bar(int64_t ts, double o, double h, double l, double c, double v) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c; b.volume = v; b.timestamp = ts;
    return b;
}

// ---- Test 1: 3-trade scenario asserting final running-max values -----------

class MaxContractsProbe : public BacktestEngine {
public:
    MaxContractsProbe() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;  // zero commission for even-trade test
    }

    void on_bar(const Bar& /*bar*/) override {
        switch (bar_index_) {
            case 0:
                // Place entry; fills at bar 1
                strategy_entry("LE", true, kNaN, kNaN, 3.0);
                break;
            case 2:
                // Place close; fills at bar 3 @ close=100 (even trade)
                strategy_close("LE");
                break;
            case 4:
                // Place short entry; fills at bar 5
                strategy_entry("SE", false, kNaN, kNaN, 5.0);
                break;
            case 6:
                // Place close; fills at bar 7 @ close=150
                strategy_close("SE");
                break;
            case 8:
                // Place long entry; fills at bar 9
                strategy_entry("LE2", true, kNaN, kNaN, 2.0);
                break;
            case 10:
                // Place close; fills at bar 11 @ close=170
                strategy_close("LE2");
                break;
            default:
                break;
        }
    }
};

static void test_max_contracts_held() {
    std::printf("test_max_contracts_held\n");

    // 12 bars. Price at close drives fill:
    //   bar 1  close=100: long fills here (entry price=100)
    //   bar 3  close=100: long closes here (exit price=100) -> even trade
    //   bar 5  close=200: short fills here (entry price=200)
    //   bar 7  close=150: short closes here (exit price=150) -> profit
    //   bar 9  close=180: long fills here (entry price=180)
    //   bar 11 close=170: long closes here (exit price=170) -> loss
    std::vector<Bar> bars = {
        mk_bar(1000,  100.0, 105.0,  95.0, 100.0, 1000.0),  // 0: place LE
        mk_bar(2000,  100.0, 110.0,  98.0, 100.0, 1000.0),  // 1: LE fills @ 100, hold
        mk_bar(3000,  100.0, 102.0,  98.0, 100.0, 1000.0),  // 2: place close
        mk_bar(4000,  100.0, 103.0,  97.0, 100.0, 1000.0),  // 3: close @ 100 (even trade)
        mk_bar(5000,  200.0, 210.0, 195.0, 200.0, 1000.0),  // 4: place SE short
        mk_bar(6000,  200.0, 205.0, 190.0, 200.0, 1000.0),  // 5: SE fills @ 200, hold
        mk_bar(7000,  150.0, 155.0, 148.0, 150.0, 1000.0),  // 6: place close
        mk_bar(8000,  150.0, 155.0, 148.0, 150.0, 1000.0),  // 7: cover @ 150 (profit)
        mk_bar(9000,  180.0, 185.0, 175.0, 180.0, 1000.0),  // 8: place LE2
        mk_bar(10000, 180.0, 182.0, 178.0, 180.0, 1000.0),  // 9: LE2 fills @ 180, hold
        mk_bar(11000, 170.0, 175.0, 165.0, 170.0, 1000.0),  // 10: place close
        mk_bar(12000, 170.0, 173.0, 167.0, 170.0, 1000.0),  // 11: exit @ 170 (loss)
    };

    MaxContractsProbe eng;
    eng.run(bars.data(), (int)bars.size());

    // Verify 3 trades were recorded
    CHECK(eng.trade_count() == 3);

    // max_contracts_held_all   = 5 (short leg, bars 5-7)
    CHECK(near(eng.max_contracts_held_all(), 5.0));
    // max_contracts_held_long  = 3 (first long leg, bars 1-3)
    CHECK(near(eng.max_contracts_held_long(), 3.0));
    // max_contracts_held_short = 5 (short leg, bars 5-7)
    CHECK(near(eng.max_contracts_held_short(), 5.0));
    // eventrades = 1 (bar 3: entry=100, exit=100, zero commission)
    CHECK(eng.eventrades() == 1);
}

// ---- Test 2: FLAT-only run leaves all counters at 0 -----------------------

class FlatOnlyProbe : public BacktestEngine {
public:
    FlatOnlyProbe() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
    }
    void on_bar(const Bar& /*bar*/) override {}
};

static void test_flat_state_no_update() {
    std::printf("test_flat_state_no_update\n");

    std::vector<Bar> bars(5);
    for (int i = 0; i < 5; ++i)
        bars[i] = mk_bar((int64_t)(i + 1) * 1000, 100.0, 105.0, 95.0, 100.0, 1000.0);

    FlatOnlyProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(near(eng.max_contracts_held_all(), 0.0));
    CHECK(near(eng.max_contracts_held_long(), 0.0));
    CHECK(near(eng.max_contracts_held_short(), 0.0));
    CHECK(eng.eventrades() == 0);
}

// ---- Test 3: multiple even trades accumulate eventrades count --------------
// Entry on bar N (fills N+1), close on bar N+1 (fills N+2).
// Pattern: place_entry on bar 0, place_close on bar 1; fills on bars 1 and 2.
// 8 bars: entries on 0,2,4 — fills on 1,3,5 — closes on 1,3,5 — fills on 2,4,6.
// That gives 3 round-trips completed by bar 6, bar 7 is flat.

class MultiEventProbe : public BacktestEngine {
public:
    MultiEventProbe() {
        initial_capital_ = 100000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
    }

    // Place entry on even bars, close on odd bars; all fills at price=100.
    // Round-trip 1: entry bar 0, close bar 1 (fills 1 and 2)
    // Round-trip 2: entry bar 2, close bar 3 (fills 3 and 4)
    // Round-trip 3: entry bar 4, close bar 5 (fills 5 and 6)
    void on_bar(const Bar& /*bar*/) override {
        if (bar_index_ == 0 || bar_index_ == 2 || bar_index_ == 4) {
            strategy_entry("LE", true, kNaN, kNaN, 1.0);
        }
        if (bar_index_ == 1 || bar_index_ == 3 || bar_index_ == 5) {
            strategy_close("LE");
        }
    }
};

static void test_multiple_eventrades() {
    std::printf("test_multiple_eventrades\n");

    // 8 bars, all at price=100. 3 round-trips each at 100 -> 100, even trades.
    std::vector<Bar> bars(8);
    for (int i = 0; i < 8; ++i)
        bars[i] = mk_bar((int64_t)(i + 1) * 1000, 100.0, 105.0, 95.0, 100.0, 1000.0);

    MultiEventProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 3);
    CHECK(eng.eventrades() == 3);
    CHECK(near(eng.max_contracts_held_long(), 1.0));
    CHECK(near(eng.max_contracts_held_short(), 0.0));
}

} // namespace

int main() {
    test_max_contracts_held();
    test_flat_state_no_update();
    test_multiple_eventrades();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
