/*
 * test_margin_call_trail_exit_chronology.cpp — finding-308 extended to TRAIL
 * exits.
 *
 * The chronological pre-exit forced-liquidation slice was gated on
 * `exit_path_fill = !exit_fill.is_trail`: a TRAIL fill was excluded because
 * the hook derived the exit's chronology from first_touch_position(fill
 * price), and a trail's fill price is not a resting level — its first path
 * touch is not necessarily its fill moment (the trail must arm first). The
 * exclusion failed closed and dropped every margin-call slice on a bar whose
 * adverse extreme precedes a trailing exit.
 *
 * resolve_exit_path_fill now reports the fill's ACTUAL path position, so the
 * chronology is exact for every intrabar path fill and the trail leg no
 * longer needs excluding.
 *
 * Exemplar (boztilkiserhan serhan1 WMA/RSI trailing scalp, ETHUSDT.P 15m,
 * 2025-10-19 08:15 UTC — bar O 3886.31 / H 3960 / L 3810 / C 3873.57, short
 * 2.119 @ 3879.36 carried in):
 *
 *   TV     — Margin call 0.234 @ 3960 (the adverse high), then "Exit Short"
 *            closes the remaining 1.885 @ 3821.06 on the H->L leg.
 *   Engine — one row: the whole 2.119 @ 3821.06, no margin call.
 *
 * Fixtures:
 *   A. HIGH-first bar, trail fills after the high -> slice 0.234 @ 3960 and
 *      the trail closes the remainder 1.885 @ 3821.06 (the tape shape).
 *   B. LOW-first bar with the same deficit at the high -> the trail fills
 *      BEFORE the extreme on the path -> no slice (fail-closed chronology
 *      is preserved, the hook is not simply switched on for trails).
 *   C. Emulator off -> nothing fires.
 *   D. #148 regression pin: an EXPLICIT trail_offset=0 must not retro-arm
 *      from the carried post-entry extreme, while an OMITTED trail_offset
 *      still does.
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

static int margin_call_rows(const MCEngine& eng) {
    int count = 0;
    for (int i = 0; i < eng.trade_count(); ++i) {
        if (eng.exit_comment(i) == std::string("Margin call")) ++count;
    }
    return count;
}

// The tape's carried short: 2.119 contracts @ 3879.36, 1x margin, no
// commission. initial_capital 8330.26 is chosen so the deficit at the
// adverse high 3960 reproduces TV's slice bit-exactly:
//
//   equity(3960) = 8330.26 + (3879.36 - 3960) * 2.119 = 8159.38384
//   q_min        = 2.119 - 8159.38384 / 3960          = 0.05854953...
//   floor(0.0001)                                     = 0.0585
//   4x                                                = 0.2340
//
// The trailing exit is the strategy's own shape: trail_points in ticks with
// an EXPLICIT trail_offset = 0 (TV's exit-at-activation trail). 5830 ticks
// at mintick 0.01 puts the activation at 3879.36 - 58.30 = 3821.06 — the
// tape's own trail-exit price.
class TrailChronologyShortProbe : public MCEngine {
public:
    explicit TrailChronologyShortProbe(bool disable_mc = false,
                                       double trail_offset = 0.0)
        : trail_offset_(trail_offset) {
        initial_capital_ = 8330.26;
        default_qty_type_ = QtyType::FIXED;
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
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/2.119);
        } else if (bar_index_ == 1) {
            // Armed while the position is live; rests for the event bar.
            strategy_exit("X", "S", /*limit=*/kNaN, /*stop=*/kNaN,
                          /*trail_points=*/5830.0,
                          /*trail_offset=*/trail_offset_,
                          /*trail_price=*/kNaN);
        }
    }

private:
    double trail_offset_;
};

static std::vector<Bar> seed_bars(const Bar& event_bar) {
    return {
        mk_bar(1000, 3879.36, 3879.36, 3879.36, 3879.36, 1.0),  // 0: signal
        mk_bar(2000, 3879.36, 3879.36, 3879.36, 3879.36, 1.0),  // 1: fill+arm
        event_bar,                                              // 2: event
    };
}

// The tape bar. |3960 - 3886.31| = 73.69 < |3886.31 - 3810| = 76.31, so the
// open is nearer the high: path O -> H -> L -> C. The adverse high sits at
// path position 1.0, the trail fills on the H->L leg at ~1.926.
static Bar tape_event_bar() {
    return mk_bar(3000, 3886.31, 3960.0, 3810.0, 3873.57, 1.0);
}

// ---- A: the tape shape — slice at the high, trail closes the remainder ----

static void test_trail_exit_slices_at_adverse_extreme_first() {
    std::printf("test_trail_exit_slices_at_adverse_extreme_first\n");
    std::vector<Bar> bars = seed_bars(tape_event_bar());

    TrailChronologyShortProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 2);
    CHECK(margin_call_rows(eng) == 1);
    CHECK(eng.exit_comment(0) == std::string("Margin call"));
    CHECK(near(eng.trade_size(0), 0.234, 1e-9));
    CHECK(near(eng.exit_price(0), 3960.0));
    CHECK(near(eng.entry_price(0), 3879.36));
    CHECK(eng.exit_bar(0) == 2);
    CHECK(eng.exit_comment(1) != std::string("Margin call"));
    CHECK(near(eng.trade_size(1), 1.885, 1e-9));
    CHECK(near(eng.exit_price(1), 3821.06));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- B: a trail that fills BEFORE the extreme stays quiet -----------------

static void test_trail_exit_before_extreme_stays_quiet() {
    std::printf("test_trail_exit_before_extreme_stays_quiet\n");
    // LOW-first bar: |3960 - 3830| = 130 > |3830 - 3810| = 20, so the path
    // is O -> L -> H -> C and the trail fills at 3821.06 on the O->L leg
    // (position ~0.45), before the adverse high at position 2.0. The
    // deficit at that high is the SAME as fixture A — only the chronology
    // differs, and it must keep the slice suppressed.
    std::vector<Bar> bars = seed_bars(
        mk_bar(3000, 3830.0, 3960.0, 3810.0, 3900.0, 1.0));

    TrailChronologyShortProbe eng;
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 2.119, 1e-9));
    CHECK(near(eng.exit_price(0), 3821.06));
    CHECK(near(eng.position_size(), 0.0));
}

// ---- C: emulator off -> nothing fires -------------------------------------

static void test_trail_chronology_disabled_emulator_stays_quiet() {
    std::printf("test_trail_chronology_disabled_emulator_stays_quiet\n");
    std::vector<Bar> bars = seed_bars(tape_event_bar());

    TrailChronologyShortProbe eng(/*disable_mc=*/true);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.trade_count() == 1);
    CHECK(margin_call_rows(eng) == 0);
    CHECK(near(eng.trade_size(0), 2.119, 1e-9));
    CHECK(near(eng.exit_price(0), 3821.06));
}

// ---- D: #148 arming semantics stay put ------------------------------------

// An EXPLICIT trail_offset=0 is TV's one-shot exit-at-activation trail and
// must NOT retro-arm from the carried post-entry extreme (#148, a6e46ca);
// an OMITTED trail_offset still does. Both cells share this fixture: the
// trail is armed on bar 1 (carried best = bar 1 close 3810, already past the
// 3821.06 activation for a short), and bar 2 opens ABOVE the activation and
// never trades down to it. Only a carried armed state can fill there.
static void test_zero_offset_arming_semantics_unchanged() {
    std::printf("test_zero_offset_arming_semantics_unchanged\n");
    std::vector<Bar> bars = {
        mk_bar(1000, 3879.36, 3879.36, 3879.36, 3879.36, 1.0),
        mk_bar(2000, 3879.36, 3879.36, 3800.00, 3810.00, 1.0),
        mk_bar(3000, 3830.00, 3835.00, 3825.00, 3832.00, 1.0),
        mk_bar(4000, 3832.00, 3836.00, 3826.00, 3833.00, 1.0),
    };

    // Explicit zero: no retro-arm -> the position is still open at the end.
    TrailChronologyShortProbe explicit_zero(/*disable_mc=*/false,
                                            /*trail_offset=*/0.0);
    explicit_zero.run(bars.data(), (int)bars.size());
    CHECK(explicit_zero.trade_count() == 0);
    CHECK(near(explicit_zero.position_size(), -2.119, 1e-9));

    // Omitted offset: the carried extreme keeps the activation armed, so
    // the exit fills at bar 2's open.
    TrailChronologyShortProbe omitted(/*disable_mc=*/false,
                                      /*trail_offset=*/kNaN);
    omitted.run(bars.data(), (int)bars.size());
    CHECK(omitted.trade_count() == 1);
    if (omitted.trade_count() == 1) {
        CHECK(near(omitted.exit_price(0), 3830.0));
        CHECK(omitted.exit_bar(0) == 2);
    }
    CHECK(near(omitted.position_size(), 0.0));
}

}  // namespace

int main() {
    std::printf("=== test_margin_call_trail_exit_chronology ===\n");

    test_trail_exit_slices_at_adverse_extreme_first();
    test_trail_exit_before_extreme_stays_quiet();
    test_trail_chronology_disabled_emulator_stays_quiet();
    test_zero_offset_arming_semantics_unchanged();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
