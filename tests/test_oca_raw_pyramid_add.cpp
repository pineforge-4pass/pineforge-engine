/*
 * test_oca_raw_pyramid_add.cpp
 *
 * Regression guard for the missing same-direction branch in
 * BacktestEngine::apply_raw_order_fill (analyzed via validation probe
 * 97a-tp-sl-bracket-isolate). Prior to the fix the helper had only two
 * branches:
 *   1. position_side_ == FLAT → open new position.
 *   2. position_side_ != requested → execute_market_exit (close).
 * A same-direction RAW_ORDER fill (position_side_ == requested) silently
 * fell through as a no-op. In real strategies this dropped legitimate
 * same-direction add-on fills — e.g. an OCA-reduce SL/TP bracket
 * placed during the prior position that survives a flip and gap-fills
 * after the flip as a same-direction add. Probe 97a lost 90 trades to
 * this bug (96.0% match → 100.0% post-fix).
 *
 * Scenario covered (minimal direct exercise of the new branch):
 *   bar 0: strategy.entry "L0" — open LONG qty=1 (fills bar 1 open).
 *   bar 1: position is LONG qty=1; place a buy-stop RAW_ORDER
 *          ("RAW_LONG_ADD") at stop=110 with qty=1.
 *   bar 2: opens at 112 (gap above stop) → buy-stop gap-fills at 112 as
 *          a SAME-direction LONG RAW_ORDER fill, routed through the new
 *          pyramid-add branch in apply_raw_order_fill.
 *
 * Expected (post-fix): position is LONG qty=2 with both L0 and
 * RAW_LONG_ADD as pyramid entries.
 *
 * Pre-fix bug: position would be LONG qty=1 with only L0 — RAW_LONG_ADD
 * would have been a silent no-op.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

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

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

namespace {

// Probe driving the leftover-bracket scenario. Each phase uses
// strategy_order so all fills route through apply_raw_order_fill.
class LeftoverBracketProbe : public BacktestEngine {
public:
    // Expose protected state for assertions.
    PositionSide live_side() const { return position_side_; }
    double live_qty() const { return position_qty_; }
    int live_entry_count() const { return position_entry_count_; }
    std::vector<std::string> live_pyramid_entry_ids() const {
        std::vector<std::string> ids;
        for (const auto& pe : pyramid_entries_) ids.push_back(pe.entry_id);
        return ids;
    }
    std::vector<std::string> closed_trade_entry_ids() const {
        std::vector<std::string> ids;
        for (const auto& t : trades_) ids.push_back(t.entry_id);
        return ids;
    }

    LeftoverBracketProbe() {
        initial_capital_ = 1'000'000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        slippage_ = 0;
        commission_value_ = 0;
        pyramiding_ = 2;  // allow one pyramid-add on top of L0
    }

    void on_bar(const Bar& bar) override {
        (void)bar;
        if (bar_index_ == 0) {
            // Open long via strategy.entry (fills at bar 1 open as MARKET).
            strategy_entry("L0", /*is_long=*/true,
                           std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::quiet_NaN(),
                           1.0,
                           "long entry");
        }
        if (bar_index_ == 1) {
            // L0 has filled — position is LONG qty=1. Place a same-
            // direction RAW_ORDER buy-stop @ 110 with
            // ``created_position_side = LONG``. The order is the
            // pyramid-add candidate. Stop sits below bar 2 open so the
            // gap-fill triggers immediately on bar 2 process.
            strategy_order("RAW_LONG_ADD", /*is_long=*/true, 1.0,
                           /*limit=*/std::numeric_limits<double>::quiet_NaN(),
                           /*stop=*/110.0,
                           "", 0);
        }
    }
};

}  // namespace

static void test_leftover_bracket_pyramid_add() {
    std::printf("test_leftover_bracket_pyramid_add\n");
    LeftoverBracketProbe p;

    // Bars: bar 1 fills L0 (LONG qty=1), then on_bar places the RAW_ORDER
    // buy-stop @ 110. Bar 2 gaps up to 112 ≥ 110 → buy-stop gap-fills at
    // open=112 as a SAME-direction (LONG) RAW_ORDER fill — exercising
    // the new pyramid-add branch.
    Bar bars[5] = {
        {100, 102,  98, 100, 1000,  60'000},  // bar 0: place L0 long
        {100, 105,  99, 102, 1000, 120'000},  // bar 1: L0 fills → LONG; place RAW_LONG_ADD
        {112, 118, 111, 115, 1000, 180'000},  // bar 2: gap up to 112; buy-stop fires → pyramid-add
        {115, 117, 113, 116, 1000, 240'000},
        {116, 118, 113, 117, 1000, 300'000},
    };
    p.run(bars, 5);

    // Post-fix expectation: live position is LONG qty=2 — L0 (initial
    // entry, qty=1) plus RAW_LONG_ADD (the same-direction RAW_ORDER
    // pyramid-add, qty=1).
    //
    // Pre-fix: position would be LONG qty=1 with only L0 live, because
    // apply_raw_order_fill silently dropped the same-direction fill.
    CHECK(p.live_side() == PositionSide::LONG);
    CHECK(near(p.live_qty(), 2.0));
    CHECK(p.live_entry_count() == 2);
    auto live_ids = p.live_pyramid_entry_ids();
    bool initial_live = false;
    bool raw_add_live = false;
    for (const auto& id : live_ids) {
        if (id == "L0") initial_live = true;
        if (id == "RAW_LONG_ADD") raw_add_live = true;
    }
    CHECK(initial_live);
    CHECK(raw_add_live);
}

int main() {
    test_leftover_bracket_pyramid_add();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
