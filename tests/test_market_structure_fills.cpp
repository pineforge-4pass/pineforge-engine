/*
 * test_market_structure_fills.cpp — tick-size robustness for the directional
 * stop snap, at instruments other than the corpus's single 0.01-tick crypto pair.
 *
 * Production-readiness probe (WS1/#7). Engine-only.
 *
 * Skeptic's objection: "every probe runs at mintick 0.01. Does the directional
 * stop-entry snap (long ceil / short floor) actually work at futures 0.25, gold
 * 0.1, FX 0.00001?" The whole corpus + every existing ctest with a sub-tick
 * SHORT stop uses on-grid prices, so the is_long_stop=false FLOOR branch is
 * never asserted at a sub-tick price. This pins it, plus the cross-mintick
 * parametricity of the snap, plus one end-to-end short-stop fill.
 */

#include <cmath>
#include <cstdio>
#include <limits>

#include <pineforge/engine.hpp>
#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>

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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

class SnapProbe : public BacktestEngine {
public:
    void on_bar(const Bar&) override {}   // helper-only; never run
    void set_mintick(double m) { syminfo_mintick_ = m; }
    // long stop snaps UP (ceil), short stop snaps DOWN (floor).
    double dsnap(double price, bool is_long_stop) const {
        return round_to_mintick_directional(price, is_long_stop);
    }
};
}  // namespace

// Directional snap math at four real instrument tick sizes.
static void test_directional_snap_multi_mintick() {
    std::printf("test_directional_snap_multi_mintick\n");
    SnapProbe p;

    // crypto 0.01: long ceil, short floor.
    p.set_mintick(0.01);
    CHECK(near(p.dsnap(100.006, /*long=*/true), 100.01));
    CHECK(near(p.dsnap(99.994, /*long=*/false), 99.99));   // FLOOR branch (uncovered)

    // ES futures 0.25.
    p.set_mintick(0.25);
    CHECK(near(p.dsnap(100.30, true), 100.50));            // ceil to next quarter
    CHECK(near(p.dsnap(100.30, false), 100.25));           // floor to quarter

    // gold 0.1.
    p.set_mintick(0.1);
    CHECK(near(p.dsnap(1635.04, true), 1635.10));
    CHECK(near(p.dsnap(1635.04, false), 1635.00));

    // FX 0.00001 (5-dp).
    p.set_mintick(0.00001);
    CHECK(near(p.dsnap(1.234566, true), 1.23457));
    CHECK(near(p.dsnap(1.234566, false), 1.23456));
}

// Parametricity: a value 0.4 ticks above a grid line snaps long->+1 line,
// short->same line, at every mintick — the snap is structurally linear in tick.
static void test_snap_parametric_across_mintick() {
    std::printf("test_snap_parametric_across_mintick\n");
    SnapProbe p;
    double minticks[3] = { 0.25, 0.1, 0.0001 };
    for (double m : minticks) {
        p.set_mintick(m);
        double line = 100.0;                  // an exact grid multiple at all these m
        double v = line + 0.4 * m;            // 0.4 tick above the line
        CHECK(near(p.dsnap(v, /*long=*/true), line + m));   // ceil -> next line
        CHECK(near(p.dsnap(v, /*long=*/false), line));      // floor -> this line
    }
}

// End-to-end: short stop entry at a sub-tick price floors, and the fill lands
// on the snapped grid value (proves the path uses the snap, not just the helper).
class ShortStopRealize : public BacktestEngine {
public:
    ShortStopRealize() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0; commission_value_ = 0; pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0)
            strategy_entry("S", false, std::numeric_limits<double>::quiet_NaN(),
                           /*stop=*/99.994, 1.0, "short stop sub-tick");
        if (bar_index_ == 3 && position_side_ == PositionSide::SHORT)
            strategy_close("S", "close");
    }
};

static void test_short_stop_entry_price_is_floored() {
    std::printf("test_short_stop_entry_price_is_floored\n");
    ShortStopRealize p;
    Bar bars[6] = {
        {100, 100.5, 99.5, 100, 1000, 900'000},
        {100, 100.5, 99.0, 99.5, 1000, 1'800'000},  // fill short @ floored stop 99.99
        {99,  99.5,  98.5, 99,   1000, 2'700'000},
        {99,  99.5,  98.5, 99,   1000, 3'600'000},   // close
        {99,  99.5,  98.5, 99,   1000, 4'500'000},
        {99,  99.5,  98.5, 99,   1000, 5'400'000},
    };
    p.run(bars, 6);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 99.99));   // floored, not 99.994
    }
}

int main() {
    test_directional_snap_multi_mintick();
    test_snap_parametric_across_mintick();
    test_short_stop_entry_price_is_floored();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
