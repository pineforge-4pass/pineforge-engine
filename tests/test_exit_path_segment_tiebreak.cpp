/*
 * test_exit_path_segment_tiebreak.cpp — pin same-bar bracket exit priority.
 *
 * Production-readiness probe (WS1/#2). Engine-only analytic OHLC oracle, no TV.
 *
 * A skeptic's objection: "when a single bar breaches BOTH my take-profit and
 * stop-loss, which one fills? Is the answer deterministic, or statistical luck?"
 *
 * The engine resolves a bracket along a 4-waypoint OHLC path:
 *   - bar_path_uses_high_first() == (|H-O| < |O-L|): high is closer to open.
 *     true  -> O -> H -> L -> C
 *     false -> O -> L -> H -> C   (ties resolve LOW-first)
 *   - resolve_exit_path_fill() fills at the FIRST level crossed along that path.
 *   - an exit stop that GAPS past the open fills at the bar open.
 *
 * Each case below is a closed-form function of O/H/L with slippage=0 and
 * mintick 0.01, so every expected exit price sits exactly on the tick grid.
 * Long position entered at 100; TP (limit) = 102, SL (stop) = 98.
 */

#include <cmath>
#include <cstdio>
#include <limits>
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

// Long entry @100 on bar0 (market, fills bar1 open=100), bracket exit armed on
// bar1, resolved on bar2 (the tie-break OHLC under test).
class BracketProbe : public BacktestEngine {
public:
    double tp_, sl_;
    BracketProbe(double tp, double sl) : tp_(tp), sl_(sl) {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 1;
        syminfo_mintick_ = 0.01;
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("L", true, kNaN, kNaN, 1.0, "enter");
        }
        if (bar_index_ == 1 && position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", /*limit=*/tp_, /*stop=*/sl_);
        }
    }
};

Bar mk(double o, double h, double l, double c, int64_t ts) {
    Bar b; b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts; return b;
}

// Run one scenario: entry bar, arm bar (open=100), tie-break bar, settle bar.
// Returns the single closed trade's exit price (NaN if no closed trade).
double run_case(double tp, double sl, Bar tiebreak) {
    BracketProbe p(tp, sl);
    Bar bars[4] = {
        mk(100, 100.5, 99.5, 100, 900'000),    // bar0: place market entry
        mk(100, 100.5, 99.5, 100, 1'800'000),  // bar1: entry fills @100, arm exit
        tiebreak,                               // bar2: resolve along OHLC path
        mk(100, 100.5, 99.5, 100, 3'600'000),  // bar3: settle
    };
    p.run(bars, 4);
    if (p.trade_count() < 1) return kNaN;
    return p.get_trade(0).exit_price;
}

}  // namespace

// Case A — high-first, take-profit on the up leg.
// (100,102.4,97.0): |H-O|=2.4 < |O-L|=3.0 -> O->H crosses TP@102 first.
static void test_high_first_tp() {
    std::printf("test_high_first_tp\n");
    double px = run_case(102, 98, mk(100, 102.4, 97.0, 100, 2'700'000));
    CHECK(near(px, 102.0));
}

// Case B — high-first, but the up leg misses TP; SL fills on the down leg.
// (100,101.0,98.0): |H-O|=1.0 < |O-L|=2.0 -> O->H (no TP) -> H->L crosses SL@98.
static void test_high_first_falls_through_to_sl() {
    std::printf("test_high_first_falls_through_to_sl\n");
    double px = run_case(102, 98, mk(100, 101.0, 98.0, 100, 2'700'000));
    CHECK(near(px, 98.0));
}

// Case C — symmetric tie resolves LOW-first, so SL fills before TP.
// (100,103,97): |H-O|=3 == |O-L|=3 -> tie -> O->L crosses SL@98 before the H
// leg could reach TP@102. This pins the unguarded "(or tied)" low-first branch.
static void test_symmetric_tie_low_first_sl() {
    std::printf("test_symmetric_tie_low_first_sl\n");
    double px = run_case(102, 98, mk(100, 103.0, 97.0, 100, 2'700'000));
    CHECK(near(px, 98.0));
}

// Case D — exit stop gaps past the open: fill at the bar open, not the level.
// open=96 <= SL@98 -> long stop gaps -> fill @96.
static void test_gap_through_stop_fills_at_open() {
    std::printf("test_gap_through_stop_fills_at_open\n");
    double px = run_case(102, 98, mk(96.0, 99.0, 95.0, 97.0, 2'700'000));
    CHECK(near(px, 96.0));
}

int main() {
    test_high_first_tp();
    test_high_first_falls_through_to_sl();
    test_symmetric_tie_low_first_sl();
    test_gap_through_stop_fills_at_open();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
