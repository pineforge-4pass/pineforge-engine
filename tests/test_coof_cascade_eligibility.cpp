/*
 * KI-67: calc_on_order_fills cascade eligibility replaces the fill-event budget.
 *
 * TradingView has NO per-bar fill-event budget. The historical 4-tick path is
 * O -> W1 -> W2 -> C (W1/W2 are the bar's two extremes in proximity order).
 * Order eligibility on that path splits by provenance:
 *
 *   - Orders RESTING at bar start, and orders placed by the BAR-OPEN fill
 *     recalc (the recalc chain triggered by a fill AT the open tick), get
 *     STANDARD semantics: exact-level fills anywhere along the remaining path.
 *   - Orders placed by a MID-BAR fill recalc ("cascade orders") are eligible
 *     ONLY at the remaining EXTREME waypoints (W1/W2): market orders fill AT
 *     the next extreme (or ROLL to next-bar open when only C remains);
 *     stop/limit orders gap-fill ONLY at an extreme waypoint tick price (no
 *     intra-segment exact-level interpolation, and NEVER at C). A cascade
 *     order that does not fill this bar converts to a normal resting order.
 *
 * These fixtures pin the two divergences the fixed 4-event budget produced:
 *   R1/R2 — cascade PRICED / MARKET orders over-thread onto the W2->C segment
 *           and the C tick (the +302 class); the new rule holds them to the
 *           remaining extreme or rolls them to the next bar.
 *   R3    — the budget truncates legitimate busy-bar RESTING-order fills TV
 *           allows (aureate's deficit direction); the new rule fills them all.
 * The G-rows lock behaviour that must NOT change: bar-open-recalc standard
 * semantics, the flag-off legacy path, and the magnifier (real lower-TF) path,
 * which owns its own tick semantics and is scoped OUT of the cascade gate.
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
    int open_lot_count() const { return static_cast<int>(pyramid_entries_.size()); }
    std::vector<double> open_lot_prices() const {
        std::vector<double> out;
        for (const auto& lot : pyramid_entries_) out.push_back(lot.price);
        return out;
    }
};

// ── R1 ────────────────────────────────────────────────────────────────
// A cascade bracket whose take-profit level lies STRICTLY inside the final
// W2->C segment. Path (H near): O=100 -> W1=101(H) -> W2=90(L) -> C=95.
//   E@100 (bar-open) -> tp bracket @101 exits at W1 -> re-enter E@90 (cascade
//   market, fills at extreme W2) -> tp bracket @93 (cascade; 90 < 93 < 95).
// Fixed 4-event engine exact-level fills that second bracket at 93 ON the
// W2->C segment (exit_bar == 1). The new rule holds it: no extreme remains
// after W2, C is ineligible, so it converts to resting and fills on bar 2.
class CascadeBracketW2CProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("E", true);
            return;
        }
        if (position_side_ == PositionSide::LONG) {
            double tp = trades_.empty() ? 101.0 : 93.0;
            strategy_exit("X", "E", /*limit=*/tp, kNaN);
        } else if (bar_index_ == 1 && reentries_ < 1) {
            strategy_entry("E", true);
            ++reentries_;
        }
    }

private:
    int reentries_ = 0;
};

void test_r1_cascade_bracket_does_not_exact_fill_on_w2_c_segment() {
    std::printf("test_r1_cascade_bracket_does_not_exact_fill_on_w2_c_segment\n");
    CascadeBracketW2CProbe p;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 101.0,  90.0,  95.0, 1000.0, 1'800'000},
        { 91.0,  94.0,  90.0,  92.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        // Cycle 1: bar-open bracket, standard semantics (unchanged).
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 101.0));
        CHECK(p.get_trade(0).entry_bar_index == 1);
        CHECK(p.get_trade(0).exit_bar_index == 1);
        // Cycle 2: cascade re-entry at W2=90; its tp=93 is inside W2->C, so the
        // exit must NOT occur on bar 1 — it converts to resting and fills bar 2.
        CHECK(near(p.get_trade(1).entry_price, 90.0));
        CHECK(near(p.get_trade(1).exit_price, 93.0));
        CHECK(p.get_trade(1).entry_bar_index == 1);
        CHECK(p.get_trade(1).exit_bar_index == 2);   // RED vs fixed budget (==1)
    }
}

// ── R2 ────────────────────────────────────────────────────────────────
// A cascade MARKET re-entry born at W2 (only C remains). Path (H near):
// O=100 -> W1=101 -> W2=90 -> C=95. E@100 (bar-open) -> stop bracket @90 exits
// at W2 -> re-enter E (cascade market): the fixed-budget engine fills it at
// the C tick (95); the new rule rolls it to the next bar's open (96).
class CascadeMarketRollProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("E", true);
            return;
        }
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "E", kNaN, 90.0);   // sl stop at the far extreme
        } else if (bar_index_ == 1 && reentries_ < 1) {
            strategy_entry("E", true);             // cascade market re-entry
            ++reentries_;
        }
    }

private:
    int reentries_ = 0;
};

void test_r2_cascade_market_only_c_remains_rolls_to_next_open() {
    std::printf("test_r2_cascade_market_only_c_remains_rolls_to_next_open\n");
    CascadeMarketRollProbe p;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 101.0,  90.0,  95.0, 1000.0, 1'800'000},
        { 96.0,  97.0,  94.0,  95.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);              // only the first cycle closes
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 90.0));
    }
    // The rolled cascade re-entry fills at bar 2's OPEN (96), not the bar-1 C
    // tick (95) the fixed budget would have used.
    CHECK(p.open_lot_count() == 1);
    if (p.open_lot_count() == 1) {
        CHECK(near(p.open_lot_prices().front(), 96.0));   // RED vs budget (95.0)
    }
    CHECK(near(p.signed_size(), 1.0));
}

// ── R3 ────────────────────────────────────────────────────────────────
// Five RESTING buy-limit orders swept by one bar. Path (L near):
// O=100 -> L=94, sweeping 99/98/97/96/95 in order. The fixed 4-event budget
// truncates the 5th (aureate's deficit direction); the new rule fills all five.
class RestingLimitSweepProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) {
            for (int i = 0; i < 5; ++i) {
                strategy_entry("E" + std::to_string(i), true,
                               /*limit=*/99.0 - i);
            }
        }
    }
};

void test_r3_more_than_four_resting_fills_are_not_budget_truncated() {
    std::printf("test_r3_more_than_four_resting_fills_are_not_budget_truncated\n");
    RestingLimitSweepProbe p;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 120.0,  94.0, 100.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.open_lot_count() == 5);          // RED vs fixed budget (==4)
    CHECK(near(p.signed_size(), 5.0));
}

// ── G3 ────────────────────────────────────────────────────────────────
// Bar-open-recalc order keeps STANDARD semantics: a bracket born when a
// carried market entry fills at the open exact-level fills at its stop within
// the same bar (green before AND after — provenance is bar-open, not mid-bar).
class BarOpenBracketProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true);
        }
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", kNaN, 99.0);   // exact-level sl
        }
    }
};

void test_g3_bar_open_recalc_bracket_keeps_exact_level_fill() {
    std::printf("test_g3_bar_open_recalc_bracket_keeps_exact_level_fill\n");
    BarOpenBracketProbe p;
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0,  90.0, 105.0, 1000.0, 1'800'000},
    };
    p.run(bars, 2);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 99.0));   // exact, not a waypoint
        CHECK(p.get_trade(0).exit_bar_index == 1);
    }
}

// ── G4 ────────────────────────────────────────────────────────────────
// calc_on_order_fills=false path is completely untouched by the cascade gate.
class LegacyProbe final : public CoofBase {
public:
    explicit LegacyProbe() : CoofBase(/*enabled=*/false) {}
    void on_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", kNaN, 99.0);
        }
    }
};

void test_g4_flag_off_path_is_legacy_identical() {
    std::printf("test_g4_flag_off_path_is_legacy_identical\n");
    LegacyProbe p;
    // Legacy (no intrabar recalc): the market entry fills at bar 1's open; the
    // sl stop placed that bar rests and fills on bar 2 — no same-bar recalc.
    Bar bars[] = {
        {100.0, 101.0,  99.0, 100.0, 1000.0,   900'000},
        {100.0, 110.0,  95.0, 105.0, 1000.0, 1'800'000},
        {100.0, 101.0,  98.0, 100.0, 1000.0, 2'700'000},
    };
    p.run(bars, 3);
    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 99.0));
        CHECK(p.get_trade(0).exit_bar_index == 2);
    }
}

// ── G5 ────────────────────────────────────────────────────────────────
// The magnifier (real lower-TF) path owns its own tick semantics and is scoped
// OUT of the historical cascade gate (the gate is guarded by
// !bar_magnifier_enabled_). A recalc-created bracket under magnifier still
// fills at its exact stop level off the real sub-bar ticks — green before AND
// after. (The full test_calc_on_order_fills magnifier suite is the broader
// magnifier-regression guard; this pins the KI-67 scoping directly.)
class MagnifierBracketProbe final : public CoofBase {
public:
    void on_bar(const Bar&) override {
        if (bar_index_ == 0 && position_side_ == PositionSide::FLAT
            && trades_.empty()) {
            strategy_entry("L", true);
        }
        if (position_side_ == PositionSide::LONG) {
            strategy_exit("X", "L", kNaN, /*stop=*/99.0);
        }
    }
};

void test_g5_magnifier_path_is_untouched_by_cascade_gate() {
    std::printf("test_g5_magnifier_path_is_untouched_by_cascade_gate\n");
    MagnifierBracketProbe p;
    // Real lower-TF magnifier data (matches the known-good recalc-bracket
    // magnifier contract): market entry fills at the entry bar's first tick
    // (100); the recalc-created stop sees only real ticks and fills at 99.
    Bar lower[] = {
        {100.0, 101.0,  99.0, 100.0, 500.0,  60'000},
        {100.0, 101.0,  99.0, 100.0, 500.0, 120'000},
        {100.0, 102.0,  98.0, 101.0, 500.0, 180'000},
        {101.0, 103.0, 100.0, 102.0, 500.0, 240'000},
    };
    p.run(lower, 4, "1", "2", /*bar_magnifier=*/true,
          /*magnifier_samples=*/4, MagnifierDistribution::ENDPOINTS);

    CHECK(p.last_error().empty());
    CHECK(p.trade_count() == 1);
    if (p.trade_count() == 1) {
        CHECK(near(p.get_trade(0).entry_price, 100.0));
        CHECK(near(p.get_trade(0).exit_price, 99.0));   // exact under magnifier
    }
}

}  // namespace

int main() {
    test_r1_cascade_bracket_does_not_exact_fill_on_w2_c_segment();
    test_r2_cascade_market_only_c_remains_rolls_to_next_open();
    test_r3_more_than_four_resting_fills_are_not_budget_truncated();
    test_g3_bar_open_recalc_bracket_keeps_exact_level_fill();
    test_g4_flag_off_path_is_legacy_identical();
    test_g5_magnifier_path_is_untouched_by_cascade_gate();

    if (tests_failed == 0) {
        std::printf("test_coof_cascade_eligibility PASSED (%d checks)\n",
                    tests_passed);
        return 0;
    }
    std::printf("test_coof_cascade_eligibility FAILED (%d failed, %d passed)\n",
                tests_failed, tests_passed);
    return 1;
}
