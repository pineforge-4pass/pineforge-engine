/*
 * KI-67 residual — strategy.exit cascade "R-cascade-gapjump" (Model S).
 *
 * PR#95 gave calc_on_order_fills ENTRY orders cascade eligibility (fill only at
 * the remaining extreme waypoints), but that machinery never reached
 * strategy.exit orders: the shipped engine rolls 100% of exits placed by a
 * mid-bar fill recalc (even the ones whose level sits exactly at a waypoint).
 * TradingView does not. A clean-room probe (pf-probe-ki67-exitlim-midseg,
 * 4,851 TV trades) pins the exact rule, Model S:
 *
 *   After an intrabar fill at price ap triggers a coof recalc, the emulator
 *   resumes the O->W1->W2->C proximity path from ap. An exit order placed at
 *   that recalc, on the historical (non-magnifier) path:
 *     (1) IN-FLIGHT leg (remainder ap -> leg-end waypoint W0): a level inside
 *         the remainder in the trigger direction gap-fills SAME BAR at W0
 *         (limits fill better than the level, stops worse). A terminal in-flight
 *         leg (-> C) never fills.
 *     (2) SUBSEQUENT legs (incl. terminal): continuous evaluation — the first
 *         crossing fills SAME BAR at the EXACT level.
 *     (3) otherwise it rolls to the next bar as an ordinary resting order.
 *
 * Geometry note (adjudication): a position's in-flight leg always moves in its
 * ENTRY direction, so only the profit-side LIMIT is ever crossed in-flight;
 * the adverse-side STOP is only crossed on a later, reversed leg (clause 2,
 * exact-level). "In-flight stop" and "subsequent-leg limit" are the
 * geometrically dead complements of that split — the rule spells them out for
 * completeness, but directed exits never hit them. The R-rows below therefore
 * realise "stop -> waypoint" as the confirmed subsequent-leg exact fill.
 *
 * Marketable-at-placement exits are handled by the pre-existing
 * coof_suppress_*_on_entry_bar mechanism (they roll), NOT by a placement-price
 * fill — see M1 and the note there.
 *
 * R-rows are RED against b6e4e35 (the exit rolls to the next bar); G/M-rows lock
 * behaviour that must NOT change.
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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

class CoofBase : public BacktestEngine {
public:
    explicit CoofBase(bool enabled = true) {
        calc_on_order_fills_ = enabled;
        initial_capital_ = 100'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        pyramiding_ = 10;
        slippage_ = 0;
        commission_value_ = 0.0;
    }
    double signed_size() const { return signed_position_size(); }
};

// A single cascade cycle: a resting long stop entry at `entry_stop` placed on
// bar 0 fills MID-BAR on bar 1 (open below it), and the fill recalc arms one
// strategy.exit whose stop/limit are supplied by the ctor. Exactly the probe's
// FRAC/WP shape.
class CascadeExitProbe final : public CoofBase {
public:
    CascadeExitProbe(double entry_stop, double exit_limit, double exit_stop)
        : entry_stop_(entry_stop), exit_limit_(exit_limit),
          exit_stop_(exit_stop) {}

    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("E", true, /*limit=*/kNaN, /*stop=*/entry_stop_);
        }
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "E", exit_limit_, exit_stop_);
        }
    }

private:
    double entry_stop_, exit_limit_, exit_stop_;
};

// The up-first work bar (bar 1): O=100 -> W1=101(H) -> W2=90(L) -> C=95.
// A long entry stop at 100.5 fills mid-bar on the in-flight leg O->W1.
std::vector<Bar> make_bars(const Bar& bar1) {
    return {
        {100.0, 100.4, 99.5, 100.0, 1000.0,   900'000},   // bar 0: below 100.5
        bar1,                                              // bar 1: cascade bar
        {100.0, 105.0, 88.0, 100.0, 1000.0, 2'700'000},   // bar 2: roll target
    };
}

// ── R1 — in-flight LIMIT gap-fills at the leg-end waypoint ───────────────────
void test_r1_inflight_limit_gap_fills_at_waypoint() {
    std::printf("test_r1_inflight_limit_gap_fills_at_waypoint\n");
    // TP=100.8 sits in the in-flight remainder (100.5, W1=101]; it gap-fills at
    // W1=101 (better than 100.8). b6e4e35 rolls -> exact 100.8 on bar 2.
    CascadeExitProbe p(/*entry_stop=*/100.5, /*exit_limit=*/100.8, /*exit_stop=*/kNaN);
    auto bars = make_bars({100.0, 101.0, 90.0, 95.0, 1000.0, 1'800'000});
    p.run(bars.data(), bars.size());

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.5));
        CHECK(near(p.get_trade(0).exit_price, 101.0));        // RED: b6e4e35 -> 100.8
        CHECK(p.get_trade(0).exit_bar_index == 1);            // RED: b6e4e35 -> 2
    }
}

// ── R2 — adverse STOP fills at the EXACT level on the subsequent leg ─────────
void test_r2_subsequent_leg_stop_exact_fill() {
    std::printf("test_r2_subsequent_leg_stop_exact_fill\n");
    // SL=95 is below the in-flight leg (never crossed rising); the reversed
    // subsequent leg W1->W2 (101->90) crosses it at the exact level 95.
    CascadeExitProbe p(/*entry_stop=*/100.5, /*exit_limit=*/kNaN, /*exit_stop=*/95.0);
    auto bars = make_bars({100.0, 101.0, 90.0, 95.0, 1000.0, 1'800'000});
    p.run(bars.data(), bars.size());

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.5));
        CHECK(near(p.get_trade(0).exit_price, 95.0));         // exact, not a waypoint
        CHECK(p.get_trade(0).exit_bar_index == 1);            // RED: b6e4e35 -> 2
    }
}

// ── R3 — STOP whose level IS the far waypoint (the coof-refill "stop-at-wp"
//         root): still a subsequent-leg exact fill, at W2=90 ─────────────────
void test_r3_subsequent_leg_stop_at_waypoint() {
    std::printf("test_r3_subsequent_leg_stop_at_waypoint\n");
    CascadeExitProbe p(/*entry_stop=*/100.5, /*exit_limit=*/kNaN, /*exit_stop=*/90.0);
    auto bars = make_bars({100.0, 101.0, 90.0, 95.0, 1000.0, 1'800'000});
    p.run(bars.data(), bars.size());

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.5));
        CHECK(near(p.get_trade(0).exit_price, 90.0));
        CHECK(p.get_trade(0).exit_bar_index == 1);            // RED: b6e4e35 -> 2
    }
}

// ── R4 — a BRACKET (stop + limit): the profit-side limit gap-fills in-flight,
//         the stop stays dormant. Exercises "limits and stops alike". ────────
void test_r4_bracket_inflight_limit_gap_fill() {
    std::printf("test_r4_bracket_inflight_limit_gap_fill\n");
    CascadeExitProbe p(/*entry_stop=*/100.5, /*exit_limit=*/100.8, /*exit_stop=*/95.0);
    auto bars = make_bars({100.0, 101.0, 90.0, 95.0, 1000.0, 1'800'000});
    p.run(bars.data(), bars.size());

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.5));
        CHECK(near(p.get_trade(0).exit_price, 101.0));        // limit at W1
        CHECK(p.get_trade(0).exit_bar_index == 1);            // RED: b6e4e35 -> 2 @100.8
    }
}

// ── G1 — a cascade exit whose level is UNREACHABLE this bar rolls (guards
//         against the fix over-firing; the terminal / no-same-bar-fill clause) ─
void test_g1_unreachable_cascade_exit_rolls() {
    std::printf("test_g1_unreachable_cascade_exit_rolls\n");
    // TP=102 is above the bar's high (101): not in the in-flight remainder and
    // never reached on any subsequent (down, then up-to-95) leg. It must roll to
    // bar 2 and fill there at the exact 102... bar 2 high is 105, so exact 102.
    CascadeExitProbe p(/*entry_stop=*/100.5, /*exit_limit=*/102.0, /*exit_stop=*/kNaN);
    auto bars = make_bars({100.0, 101.0, 90.0, 95.0, 1000.0, 1'800'000});
    p.run(bars.data(), bars.size());

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.5));
        CHECK(p.get_trade(0).exit_bar_index == 2);            // rolled (before AND after)
        CHECK(near(p.get_trade(0).exit_price, 102.0));
    }
}

// ── M1 — marketable-at-placement is SUPPRESSED and rolls, not filled at the
//         placement price. This documents the one deliberate departure from the
//         pinned-rule prose ("marketable -> fills at p"): the engine's existing
//         coof_suppress_*_on_entry_bar path owns that case and rolls it, and the
//         byte-exact coof cohort is validated with that behaviour. Green before
//         AND after — the exit-cascade change does not touch it. ─────────────
void test_m1_marketable_at_placement_is_suppressed_and_rolls() {
    std::printf("test_m1_marketable_at_placement_is_suppressed_and_rolls\n");
    // SL=100.6 is above ap=100.5: a long sell-stop already breached at
    // placement (marketable). Suppressed on the entry bar -> rolls to bar 2.
    CascadeExitProbe p(/*entry_stop=*/100.5, /*exit_limit=*/kNaN, /*exit_stop=*/100.6);
    auto bars = make_bars({100.0, 101.0, 90.0, 95.0, 1000.0, 1'800'000});
    p.run(bars.data(), bars.size());

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.5));
        CHECK(p.get_trade(0).exit_bar_index == 2);            // NOT filled at 100.5 on bar 1
    }
}

// ── G2 — a bar-OPEN-recalc exit keeps STANDARD exact-level semantics: the
//         exit-cascade gate only touches coof_born_mid_bar exits. ────────────
class BarOpenExitProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("E", true);   // market -> fills at bar 1 open (bar-open recalc)
        }
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "E", kNaN, /*stop=*/99.0);
        }
    }
};

void test_g2_bar_open_recalc_exit_exact_level_unchanged() {
    std::printf("test_g2_bar_open_recalc_exit_exact_level_unchanged\n");
    BarOpenExitProbe p;
    // Down-first bar: O=100 -> L=95 -> H=110 -> C=105. Market entry fills at the
    // open (100); the bracket armed in that bar-open recalc is NOT a cascade
    // order and exact-fills its stop at 99 on the O->L leg, same bar.
    Bar bars[] = {
        {100.0, 101.0,  99.5, 100.0, 1000.0,   900'000},
        {100.0, 110.0,  95.0, 105.0, 1000.0, 1'800'000},
        {100.0, 101.0,  98.0, 100.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 99.0));         // exact, same bar
        CHECK(p.get_trade(0).exit_bar_index == 1);
    }
}

}  // namespace

int main() {
    test_r1_inflight_limit_gap_fills_at_waypoint();
    test_r2_subsequent_leg_stop_exact_fill();
    test_r3_subsequent_leg_stop_at_waypoint();
    test_r4_bracket_inflight_limit_gap_fill();
    test_g1_unreachable_cascade_exit_rolls();
    test_m1_marketable_at_placement_is_suppressed_and_rolls();
    test_g2_bar_open_recalc_exit_exact_level_unchanged();

    if (tests_failed == 0) {
        std::printf("test_cascade_exit_gapjump PASSED (%d checks)\n", tests_passed);
        return 0;
    }
    std::printf("test_cascade_exit_gapjump FAILED (%d failed, %d passed)\n",
                tests_failed, tests_passed);
    return 1;
}
