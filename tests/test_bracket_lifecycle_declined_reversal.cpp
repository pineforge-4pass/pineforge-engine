/*
 * test_bracket_lifecycle_declined_reversal.cpp — finding-311: exit-bracket
 * LIFECYCLE across declined in-position reversal signals.
 *
 * TV rule set (stevenygabbyperez derivation, 162/162 episodes):
 *   KILL     — a declined in-position opposite MARKET reversal (the KI-54/
 *              KI-72 decline arms, the "tradeless reversal") cancels the live
 *              position's standing PRICED strategy.exit brackets. Not the
 *              "__close__" family, not stale exits bound to unfilled entries.
 *   DORMANT  — a killed bracket never matches a fill (118/118 TV stop-skips),
 *              but stays in the book.
 *   REVIVE-A — a fresh same-(id,from_entry) strategy.exit re-issue replaces
 *              the dormant bracket wholesale and arms the NEW call's prices.
 *   REVIVE-B — a margin-call PARTIAL re-registers the surviving position's
 *              dormant brackets at their LAST-ARMED (original) prices.
 *   CASCADE  — if the margin-call event price already makes a revived stop
 *              marketable, the WHOLE remaining position closes at that event
 *              price through the bracket's id (TV books the "Margin call"
 *              slice and the residual close at the same adverse extreme).
 *
 * Harness: modelled on test_declined_reversal_close_leg.cpp (Probe subclass,
 * scripted per-bar actions; initial_capital 10000, PERCENT_OF_EQUITY pct=100,
 * zero commission, qty_step 0). The canonical decline fixture is the same
 * +1-gap open: LONG 100 @100, signal close 110 (eq 11000, frozen opposite qty
 * 100), fill bar opens 111 -> required 11100 > 11000 -> KI-54 DECLINE.
 *
 * Matrix:
 *   KILL      declined reversal kills the bracket; a same-bar stop touch does
 *             not fill (RED pre-fix: the stop filled early).
 *   DORMANT   later-bar touches never fill either; position held.
 *   REVIVE-A  same-(id,from_entry) re-issue arms fresh prices and fills.
 *   ADMITTED  admitted reversal unchanged (fix inert; flip books the trade).
 *   REVIVE-B  margin-call partial revives the bracket at its original price;
 *             it fills normally on a later bar.
 *   CASCADE   revived stop marketable at the margin-call event price closes
 *             the entire remainder at that price under the bracket's id.
 *   R5        close_all co-queued with the declined reversal still fires
 *             (the "__close__" family is excluded from the kill).
 *   COOF      KI-60 kernel mirror: the dormant flag set mid-segment by an
 *             earlier candidate's decline is caught at apply time (the COOF
 *             kernel pre-classifies its candidates, so classify's Skip alone
 *             cannot see it). RED without the apply-time mirror.
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

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

static Bar mk(int64_t ts, double o, double h, double l, double c) {
    Bar b;
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1.0; b.timestamp = ts;
    return b;
}

namespace {

// Scripted per-bar actions; creation order within a bar is preserved.
enum class Op { EnterLong, EnterShort, ExitStop90, ExitStop95, ExitStop105,
                CloseAll };
struct Action { Op op; };

class Probe : public BacktestEngine {
public:
    Probe() {
        initial_capital_ = 10000.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
        commission_value_ = 0.0;
        pyramiding_ = 1;
        margin_call_enabled_ = false;
        syminfo_mintick_ = 0.01;
    }
    std::vector<std::vector<Action>> plan;   // plan[bar_index] = actions
    void on_bar(const Bar&) override {
        if (bar_index_ < 0 || bar_index_ >= (int)plan.size()) return;
        for (const auto& a : plan[bar_index_]) {
            switch (a.op) {
                case Op::EnterLong:   strategy_entry("L", true); break;
                case Op::EnterShort:  strategy_entry("S", false); break;
                case Op::ExitStop90:
                    strategy_exit("X", "L", kNaN, 90.0, kNaN, kNaN, kNaN,
                                  100.0, "");
                    break;
                case Op::ExitStop95:
                    strategy_exit("X", "L", kNaN, 95.0, kNaN, kNaN, kNaN,
                                  100.0, "");
                    break;
                case Op::ExitStop105:
                    strategy_exit("X", "L", kNaN, 105.0, kNaN, kNaN, kNaN,
                                  100.0, "");
                    break;
                case Op::CloseAll:    strategy_close_all(); break;
            }
        }
    }
    std::string x_comment(int i) const { return closed_trade_exit_comment(i); }
    std::string x_id(int i) const { return closed_trade_exit_id(i); }
    double x_price(int i) const { return closed_trade_exit_price(i); }
    double t_size(int i) const { return closed_trade_size(i); }
    int x_bar(int i) const { return closed_trade_exit_bar_index(i); }
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_side_;
    using BacktestEngine::calc_on_order_fills_;
    using BacktestEngine::margin_call_enabled_;
    using BacktestEngine::margin_short_;
};

// Canonical LONG-then-declined-reversal bars. LONG fills 100 @100 (bar1),
// bar1 closes 110 (eq 11000, frozen short qty 100 @110), and bar2 opens +1
// at 111 -> the short reversal DECLINES (11100 > 11000). `low2` shapes bar2's
// low so a fixture can touch (or avoid) the 90 stop on the decline bar.
static std::vector<Bar> decline_bars(double low2) {
    return {
        mk(1000, 100, 100, 100, 100),                 // bar0: place L
        mk(2000, 100, 112,  99, 110),                 // bar1: L fills @100; arm
        mk(3000, 111, 112, low2, 111),                // bar2: S declines @111
        mk(4000, 111, 112, low2, 111),                // bar3
        mk(5000, 111, 112, low2, 111),                // bar4
        mk(6000, 111, 111, 111, 111),                 // bar5
    };
}

}  // namespace

// KILL: the declined reversal kills the standing stop bracket. bar2 declines S
// at the 111 open and its low 89 crosses the 90 stop — a live bracket would
// fill @90. Post-fix the bracket is dormant: NO fill, LONG held.
static void test_kill_bracket_on_declined_reversal() {
    std::printf("-- KILL: declined reversal kills the standing bracket --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},                            // bar0
        {{Op::ExitStop90}, {Op::EnterShort}},         // bar1: arm X; queue S
        {}, {}, {}, {},
    };
    auto bars = decline_bars(/*low2=*/89);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);    // RED pre-fix: FLAT @90
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 0);
}

// DORMANT: repeated later-bar touches of the killed stop never fill either —
// the bracket stays in the book but never matches (118/118 TV stop-skips).
static void test_dormant_touches_never_fill() {
    std::printf("-- DORMANT: later-bar touches never fill --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::ExitStop90}, {Op::EnterShort}},
        {}, {}, {}, {},
    };
    auto bars = decline_bars(/*low2=*/89);
    bars[3] = mk(4000, 100, 100, 88, 100);            // bar3: touch again
    bars[4] = mk(5000, 100, 100, 87, 100);            // bar4: and again
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 0);
}

// REVIVE-A: a fresh same-(id,from_entry) strategy.exit re-issue replaces the
// dormant bracket wholesale and arms the NEW prices. The re-issued stop 95
// fills on the next touch bar at 95 (not at the original 90).
static void test_revive_A_reissue_arms_fresh_prices() {
    std::printf("-- REVIVE-A: same-(id,from_entry) re-issue arms fresh prices --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},                            // bar0
        {{Op::ExitStop90}, {Op::EnterShort}},         // bar1: arm X@90; queue S
        {},                                           // bar2: S declines; kill
        {{Op::ExitStop95}},                           // bar3: re-issue X@95
        {},                                           // bar4: touch -> fill @95
        {},
    };
    auto bars = decline_bars(/*low2=*/110);           // no touch on bar2
    bars[3] = mk(4000, 110, 110, 110, 110);           // bar3: quiet re-issue bar
    bars[4] = mk(5000,  96,  97,  89,  95);           // bar4: crosses 95 (and 90)
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::FLAT);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK_NEAR(p.x_price(0), 95.0, 1e-9);         // NEW price, not 90
        CHECK(p.x_id(0) == std::string("X"));
        CHECK(p.x_bar(0) == 4);
    }
}

// ADMITTED: an admitted reversal is untouched by the kill machinery — the tie
// fill (open 110 == frozen sizing price) flips the position and books the L
// round-trip exactly as before.
static void test_admitted_reversal_unchanged() {
    std::printf("-- ADMITTED: admitted reversal unchanged (fix inert) --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::ExitStop90}, {Op::EnterShort}},
        {}, {}, {}, {},
    };
    auto bars = decline_bars(/*low2=*/110);
    bars[2] = mk(3000, 110, 112, 110, 110);           // tie open -> ADMIT
    bars[3] = mk(4000, 110, 110, 110, 110);
    bars[4] = mk(5000, 110, 110, 110, 110);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::SHORT);   // flip happened
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) CHECK_NEAR(p.x_price(0), 110.0, 1e-9);
}

namespace {

// SHORT-side margin-call fixtures (REVIVE-B / CASCADE). A 5x short (margin_
// short=20) opens 100 @100; bar1 closes 90 (eq 11000, frozen long qty
// 122.22 @90); bar2 opens 91 -> the LONG reversal DECLINES (122.22*91 =
// 11122.2 > 11000; margin_long stays 100) and kills the short's bracket.
// bar3 spikes to an adverse high 170: equity 3000 < required 3400 ->
// q_min = 100 - 3000/34 = 11.7647..., slice 4x = 47.0588... (a PARTIAL),
// booked "Margin call" @170; the slice then revives the bracket at its
// original stop. The bracket is armed on the ENTRY's signal bar (the tape's
// entry-bound shape): it defers with qty=NaN and the fill side executes a
// FULL remaining close — the same full-percent default shape the cascade's
// marketability rule is pinned on.
class ShortMcProbe : public Probe {
public:
    explicit ShortMcProbe(double stop_price) : stop_price_(stop_price) {
        margin_call_enabled_ = true;
        margin_short_ = 20.0;                          // 5x short
    }
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false);
            strategy_exit("X", "S", kNaN, stop_price_, kNaN, kNaN, kNaN,
                          100.0, "");
        }
        if (bar_index_ == 1) {
            strategy_entry("L", true);                 // the reversal-to-decline
        }
    }
private:
    double stop_price_;
};

static std::vector<Bar> short_mc_bars(const Bar& post_event_bar) {
    return {
        mk(1000, 100, 100, 100, 100),                 // bar0: place S
        mk(2000, 100, 101,  99,  90),                 // bar1: S fills @100; arm
        mk(3000,  91,  91,  91,  91),                 // bar2: L declines; kill
        mk(4000, 165, 170, 160, 168),                 // bar3: MC partial @170
        post_event_bar,                               // bar4
    };
}

}  // namespace

// REVIVE-B: the margin-call PARTIAL revives the dormant bracket at its
// original price. Stop 180 is NOT marketable at the 170 event price (no
// cascade); the revived stop then fills normally on bar4's 180 touch.
static void test_revive_B_margin_call_partial_revives() {
    std::printf("-- REVIVE-B: margin-call partial revives at original price --\n");
    ShortMcProbe p(/*stop=*/180.0);
    auto bars = short_mc_bars(mk(5000, 175, 185, 170, 180));
    p.run(bars.data(), (int)bars.size());
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(p.x_comment(0) == std::string("Margin call"));
        CHECK_NEAR(p.t_size(0), 47.0588235294, 1e-6);
        CHECK_NEAR(p.x_price(0), 170.0, 1e-9);
        CHECK(p.x_bar(0) == 3);
        CHECK(p.x_id(1) == std::string("X"));         // revived bracket fill
        CHECK_NEAR(p.t_size(1), 52.9411764706, 1e-6);
        CHECK_NEAR(p.x_price(1), 180.0, 1e-9);        // ORIGINAL armed price
        CHECK(p.x_bar(1) == 4);
    }
    CHECK(p.position_side_ == PositionSide::FLAT);
}

// CASCADE: the revived stop 150 is already marketable at the 170 event price
// (short stop <= event price), so the ENTIRE remainder closes at the event
// price through the bracket's id on the same bar as the slice. Pre-revive the
// dormant stop must NOT have filled at bar3's open 165 (dormancy proof).
static void test_cascade_marketable_revived_stop() {
    std::printf("-- CASCADE: revived stop marketable at MC price closes remainder --\n");
    ShortMcProbe p(/*stop=*/150.0);
    auto bars = short_mc_bars(mk(5000, 168, 168, 168, 168));
    p.run(bars.data(), (int)bars.size());
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        CHECK(p.x_comment(0) == std::string("Margin call"));
        CHECK_NEAR(p.t_size(0), 47.0588235294, 1e-6);
        CHECK_NEAR(p.x_price(0), 170.0, 1e-9);        // NOT the 165 open
        CHECK(p.x_bar(0) == 3);
        CHECK(p.x_id(1) == std::string("X"));
        CHECK(p.x_comment(1) != std::string("Margin call"));
        CHECK_NEAR(p.t_size(1), 52.9411764706, 1e-6);
        CHECK_NEAR(p.x_price(1), 170.0, 1e-9);        // MC event price
        CHECK(p.x_bar(1) == 3);                       // same bar as the slice
    }
    CHECK(p.position_side_ == PositionSide::FLAT);
}

// R5 non-regression: a close_all co-queued with the declined reversal still
// fires — the "__close__" family (targeted AND bare) is excluded from the
// kill, exactly like it is excluded from close-leg suppression.
static void test_R5_close_all_still_fires() {
    std::printf("-- R5: close_all co-queued with declined reversal still fires --\n");
    Probe p;
    p.plan = {
        {{Op::EnterLong}},
        {{Op::ExitStop90}, {Op::EnterShort}, {Op::CloseAll}},
        {}, {}, {}, {},
    };
    auto bars = decline_bars(/*low2=*/110);
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::FLAT);    // close_all flattened
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) CHECK_NEAR(p.x_price(0), 111.0, 1e-9);
}

// COOF mirror: under calc_on_order_fills the KI-60 kernel pre-classifies its
// whole candidate set before applying any candidate, so the dormant flag set
// by the reversal's decline mid-segment is invisible to classify — the shared
// apply-time guard must catch it. bar3 declines S at the 111 open and its low
// 104 crosses the 105 stop pre-classified in the same candidate set. RED
// without the apply-time mirror (the stop fills @105 -> FLAT).
static void test_coof_kernel_mirror() {
    std::printf("-- COOF: KI-60 kernel apply-time mirror --\n");
    Probe p;
    p.calc_on_order_fills_ = true;
    p.plan = {
        {{Op::EnterLong}},                            // bar0: place L
        {},                                           // bar1: L fills @100
        {{Op::ExitStop105}, {Op::EnterShort}},        // bar2: signal @110
        {}, {}, {},
    };
    std::vector<Bar> bars = {
        mk(1000, 100, 100, 100, 100),
        mk(2000, 100, 100, 100, 100),                 // L fills @100
        mk(3000, 100, 112,  99, 110),                 // profit; X + S queued
        mk(4000, 111, 112, 104, 111),                 // +1 gap declines S; low
                                                      // crosses the 105 stop
        mk(5000, 111, 111, 111, 111),
        mk(6000, 111, 111, 111, 111),
    };
    p.run(bars.data(), (int)bars.size());
    CHECK(p.position_side_ == PositionSide::LONG);    // RED pre-mirror: FLAT
    CHECK_NEAR(p.position_qty_, 100.0, 1e-9);
    CHECK(p.trade_count() == 0);
}

int main() {
    std::printf("--- bracket_lifecycle_declined_reversal ---\n");
    test_kill_bracket_on_declined_reversal();
    test_dormant_touches_never_fill();
    test_revive_A_reissue_arms_fresh_prices();
    test_admitted_reversal_unchanged();
    test_revive_B_margin_call_partial_revives();
    test_cascade_marketable_revived_stop();
    test_R5_close_all_still_fires();
    test_coof_kernel_mirror();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
