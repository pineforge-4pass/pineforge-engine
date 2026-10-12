/*
 * test_trail_open_arm_subtick_offset.cpp — two trailing-exit rules pinned
 * against TradingView behaviour (recorded tapes are kept outside this
 * repository). Legacy test function names stay fixed; cent-tick fixtures
 * use invented prices and timestamps.
 *
 * (B) A resting trail observes the OPEN first. An open past activation arms
 * at best = open, so an adverse-first leg fills at open -/+ offset rather
 * than waiting for the favourable extreme. The cent-tick short and long
 * below independently construct high-first and low-first paths.
 *
 * (A) trail_offset in [0, 1) floors to zero ticks. Activation first reached
 * intrabar gives a one-shot fill at activation; an already-armed trail
 * follows the raw running best. Open prints versus directional levels and
 * carried placement closes distinguish zero distance from an omitted offset.
 *
 * One-bar fixtures exercise the product adapter and kernel event projection.
 * Engine fixtures include POOC entry, whose carried best starts at its close.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "trail_exit_product_probe.hpp"

using namespace pineforge;
using namespace pineforge::trail_probe;

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

bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

Bar mk(double o, double h, double l, double c, int64_t ts = 0) {
    Bar b{};
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = ts;
    return b;
}

// The one-bar product probe for a lone trailing exit (no stop / limit legs)
// resting on a NON-entry bar from the open, in the plain (non-magnifier)
// path: a position at `entry` whose carried running extreme is `best_start`.
TrailExitProjection trail_fill(const Bar& bar, PositionSide side,
                               double trail_points, double trail_offset,
                               double entry, double best_start, double mintick) {
    TrailExitScenario scenario;
    scenario.bar = bar;
    scenario.is_long = side == PositionSide::LONG;
    scenario.trail_points = trail_points;
    scenario.trail_offset = trail_offset;
    scenario.entry = entry;
    scenario.best_start = best_start;
    scenario.mintick = mintick;
    return trail_exit(scenario);
}

// ── (B) the product: the open arms the trail ──────────────────────────

void test_open_arms_trail_xauusd_long() {
    std::printf("test_open_arms_trail_xauusd_long\n");
    // Carried best = entry (POOC close fill, no entry-bar extremes folded).
    // activation = 3110.31 + 15*0.001 = 3110.325 <= open 3110.40 -> armed at
    // the open, best = open. |O-L| = 1.16 < |H-O| = 26.375 -> low-first path
    // O -> L -> H -> C; the O->L leg crosses open - 0.015 = 3110.385.
    Bar xau = mk(3110.40, 3136.775, 3109.24, 3134.46);
    TrailExitProjection f = trail_fill(xau, PositionSide::LONG,
                                       /*trail_points=*/15.0, /*trail_offset=*/15.0,
                                       /*entry=*/3110.31, /*best_start=*/3110.31,
                                       /*mintick=*/0.001);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 3110.385));
    CHECK(near(f.raw_price, 3110.385));
    CHECK(f.leg_is_trail == true);
    CHECK(f.level_fill == true);
    CHECK(f.leg_is_limit == false);
    CHECK(f.at_bar_open == false);
    // On the O->L leg: (3110.385 - 3110.40) / (3109.24 - 3110.40).
    CHECK(near(f.path_position, 0.015 / 1.16, 1e-6));
}

void test_open_arms_trail_stock_short() {
    std::printf("test_open_arms_trail_stock_short\n");
    // Invented short entry 256.00: ceil(1.7) = 2 ticks activates at 255.98.
    // Open 240.00 is past activation; floor(1.7) = 1 tick offset = 0.01.
    // |H-O| = 1.00 < |O-L| = 3.00: high-first, crossing 240.00 + 0.01.
    Bar short_bar = mk(240.00, 241.00, 237.00, 239.50);
    TrailExitProjection f = trail_fill(short_bar, PositionSide::SHORT,
                                       /*trail_points=*/1.7, /*trail_offset=*/1.7,
                                       /*entry=*/256.00, /*best_start=*/256.00,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 240.01));
    CHECK(near(f.raw_price, 240.01));
    CHECK(f.leg_is_trail == true);
    CHECK(f.at_bar_open == false);
    // On O->H: (240.01 - 240.00) / (241.00 - 240.00) = 0.01.
    CHECK(near(f.path_position, 0.01 / 1.00, 1e-6));
}

void test_open_arms_trail_ford_daily_long() {
    std::printf("test_open_arms_trail_ford_daily_long\n");
    // Invented long entry 128.00: ceil(1.5) = 2 ticks activates at 128.02.
    // Open 128.10 arms with a one-tick offset. |O-L| = 0.08 < |H-O| = 0.50,
    // so the low-first path crosses 128.10 - 0.01 = 128.09 before its high.
    Bar long_bar = mk(128.10, 128.60, 128.02, 128.40);
    TrailExitProjection f = trail_fill(long_bar, PositionSide::LONG,
                                       /*trail_points=*/1.5, /*trail_offset=*/1.5,
                                       /*entry=*/128.00, /*best_start=*/128.00,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 128.09));
    CHECK(f.leg_is_trail == true);
    // On O->L: (128.10 - 128.09) / (128.10 - 128.02) = 0.125.
    CHECK(near(f.path_position, 0.01 / 0.08, 1e-6));
}

void test_open_is_a_new_best_for_an_armed_trail() {
    std::printf("test_open_is_a_new_best_for_an_armed_trail\n");
    // Consequence of the same rule (not a separately tape-pinned value): a
    // trail already armed from the carried best (102 >= activation 101) sees
    // a gap-up open 103 as its new best, so the adverse-first leg fills at
    // 103 - 0.5 = 102.5 rather than trailing the stale 102 - 0.5 = 101.5
    // (which this bar never reaches — the old walk produced NO fill here).
    Bar gap_up = mk(103.0, 104.0, 102.4, 103.8);
    TrailExitProjection f = trail_fill(gap_up, PositionSide::LONG,
                                       /*trail_points=*/100, /*trail_offset=*/50,
                                       /*entry=*/100.0, /*best_start=*/102.0,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 102.5));
    CHECK(f.leg_is_trail == true);
    CHECK(f.level_fill == true);
}

void test_open_below_activation_does_not_arm() {
    std::printf("test_open_below_activation_does_not_arm\n");
    // Control: open 100.5 < activation 101 -> nothing changes; the trail
    // still arms at the high and retraces from it (the established
    // test_resolve_exit_trail_fills pin: fill 101.5).
    Bar bar = mk(100.5, 102, 100, 100.2);
    TrailExitProjection f = trail_fill(bar, PositionSide::LONG,
                                       /*trail_points=*/100, /*trail_offset=*/50,
                                       /*entry=*/100.0, /*best_start=*/kNaN,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 101.5));
    CHECK(f.leg_is_trail == true);
    // Low-first path O->L->H->C; the trail arms on the L->H leg and the
    // H->C leg crosses 102 - 0.5 = 101.5.
    CHECK(near(f.path_position, 2.0 + 0.5 / 1.8, 1e-6));
}

// ── (A) the product: sub-tick offset == explicit zero ─────────────────

void test_subtick_offset_fills_at_activation_eurusd_short() {
    std::printf("test_subtick_offset_fills_at_activation_eurusd_short\n");
    // trail_points 0.6 -> ceil 1t -> activation 1.08329; offsets 0 / 0.5 /
    // 0.9 / 0.6 all floor to 0 ticks -> the one-shot rule fills AT 1.08329
    // on the O->L leg (|H-O| = 0.0002 >= |O-L| = 0.00016 -> low-first path).
    // The old finite-zero distance armed at the low and filled at 1.08314.
    //
    // expectation corrected: the leg is a TRAIL crossing its level ->
    // a one-shot kernel LIMIT crossing the adapter's half-tick arm threshold
    // (1.083295, where the tick-quantized path first prints the activation),
    // because the product lowers a zero-tick-offset trail to that one-shot
    // and books the source activation from it; TradingView's 1.08329 is the
    // same number the resolver pinned.
    Bar eur = mk(1.08330, 1.08350, 1.08314, 1.08320);
    const double offsets[] = {0.0, 0.5, 0.9, 0.6};
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(eur, PositionSide::SHORT,
                                           /*trail_points=*/0.6, off,
                                           /*entry=*/1.08330, /*best_start=*/1.08330,
                                           /*mintick=*/0.00001);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 1.08329));
        CHECK(f.leg_is_limit == true);
        CHECK(f.level_fill == true);
        CHECK(near(f.raw_price, 1.083295));
        CHECK(f.at_bar_open == false);
        CHECK(near(f.path_position, 0.000005 / 0.00016, 1e-6));
    }
}

void test_subtick_offset_fills_at_activation_long() {
    std::printf("test_subtick_offset_fills_at_activation_long\n");
    // Long mirror: activation 100.03 crossed on the rising L->H leg
    // (|H-O| = 0.10 >= |O-L| = 0.05 -> low-first). Old code with 0.5: armed
    // at the high 100.10 and filled there.
    Bar bar = mk(100.00, 100.10, 99.95, 100.05);
    const double offsets[] = {0.0, 0.5, 0.9};
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(bar, PositionSide::LONG,
                                           /*trail_points=*/3.0, off,
                                           /*entry=*/100.0, /*best_start=*/100.0,
                                           /*mintick=*/0.01);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 100.03));
        CHECK(f.leg_is_limit == true);
        CHECK(near(f.raw_price, 100.025));
        // The L->H leg (segment 2 of O->L->H->C) crosses the threshold
        // halfway: (100.025 - 99.95) / (100.10 - 99.95).
        CHECK(near(f.path_position, 1.0 + 0.075 / 0.15, 1e-9));
    }
}

void test_subtick_offset_gapped_open_arms_and_rides() {
    std::printf("test_subtick_offset_gapped_open_arms_and_rides\n");
    // The bar opens past the activation (1.08320 <= 1.08329 for a short) and
    // the open ARMS the trail with best = open — for 0 / 0.5 / 0.9 alike,
    // since a sub-tick offset truncates to zero ticks. What the open then
    // does is decided by its PRINT against the level it creates (round 10
    // family AC, pins4 / pins5): the level is the open snapped directionally
    // (short: ceil) and the print is the nearest tick; an ON-GRID open is
    // both at once and is touched at the open. 1.08320 is a whole 0.00001
    // tick -> the open fills, whatever the path.
    Bar gap = mk(1.08320, 1.08340, 1.08305, 1.08330);
    const double offsets[] = {0.0, 0.5, 0.9};
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(gap, PositionSide::SHORT,
                                           /*trail_points=*/0.6, off,
                                           /*entry=*/1.08330, /*best_start=*/1.08330,
                                           /*mintick=*/0.00001);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 1.08320));
        CHECK(near(f.raw_price, 1.08320));
        CHECK(f.at_bar_open == true);
        CHECK(near(f.path_position, 0.0));
    }
    // High-first twin with the same on-grid open: the open is already a
    // touch, so neither path order changes the fill or cursor at the open.
    Bar gap_hf = mk(1.08320, 1.08330, 1.08300, 1.08310);
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(gap_hf, PositionSide::SHORT,
                                           /*trail_points=*/0.6, off,
                                           /*entry=*/1.08330, /*best_start=*/1.08330,
                                           /*mintick=*/0.00001);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 1.08320));
        CHECK(near(f.raw_price, 1.08320));
        CHECK(f.at_bar_open == true);
        CHECK(near(f.path_position, 0.0));
    }
    // SUB-TICK open twins — the other side of the rule is decided by the
    // open's PRINT (nearest tick, floor(x / tick + 0.5) in doubles) against
    // the ceiled level. 1.083205 / 0.00001 = 108320.49999.. prints 1.08320,
    // one tick BELOW the ceiled level 1.08321 (away from a short's stop):
    // nothing is touched at the open and the trail rides the raw running
    // best — on this low-first bar (|O-L| = 0.000155 < |H-O| = 0.000195) the
    // O->L leg lowers the best to the 1.08305 low and the L->H leg crosses
    // it. The invented cent-tick fixtures above independently test open arming.
    //
    // expectation corrected: the kernel cursor is the L->H leg's start
    // (is_trail, path position 1.0) -> the bar's OPEN point (path position
    // 0.0), because the product rests this shape as a one-shot whose region
    // the open already sits in, and the adapter's terms policy answers with
    // TradingView's ridden level; the booked 1.08305 is unchanged.
    Bar gap_sub = mk(1.083205, 1.08340, 1.08305, 1.08330);
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(gap_sub, PositionSide::SHORT,
                                           /*trail_points=*/0.6, off,
                                           /*entry=*/1.08330, /*best_start=*/1.08330,
                                           /*mintick=*/0.00001);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 1.08305));
        CHECK(near(f.raw_price, 1.083205));
        CHECK(f.at_bar_open == true);
        CHECK(near(f.path_position, 0.0));
    }
    // 1.083215 / 0.00001 = 108321.5 prints 1.08322 = the ceiled level: the
    // print sits AT the short's stop and the exit fills at the open, a level
    // fill the consumer ceils to 1.08322, whatever the path.
    Bar gap_sub_at = mk(1.083215, 1.08340, 1.08305, 1.08330);
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(gap_sub_at, PositionSide::SHORT,
                                           /*trail_points=*/0.6, off,
                                           /*entry=*/1.08330, /*best_start=*/1.08330,
                                           /*mintick=*/0.00001);
        CHECK(f.filled == true);
        CHECK(near(f.raw_price, 1.083215));
        CHECK(near(f.exit_price, 1.08322));
        CHECK(f.at_bar_open == true);
        CHECK(near(f.path_position, 0.0));
    }
}

void test_whole_tick_offsets_keep_floored_trailing_distance() {
    std::printf("test_whole_tick_offsets_keep_floored_trailing_distance\n");
    // Guard against over-reach: 1.0 and 1.4 ticks both floor to ONE tick
    // (the nils123456-orb / legalrice rule) and keep trailing the running
    // extreme: armed at the low 1.08314, filled at 1.08315 on the L->H leg.
    Bar eur = mk(1.08330, 1.08350, 1.08314, 1.08320);
    const double offsets[] = {1.0, 1.4};
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(eur, PositionSide::SHORT,
                                           /*trail_points=*/0.6, off,
                                           /*entry=*/1.08330, /*best_start=*/1.08330,
                                           /*mintick=*/0.00001);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 1.08315));
        CHECK(f.leg_is_trail == true);
        CHECK(f.level_fill == true);
        CHECK(near(f.path_position, 1.0 + 0.00001 / 0.00036, 1e-6));
    }
}

void test_subtick_offset_arms_from_the_carried_best() {
    std::printf("test_subtick_offset_arms_from_the_carried_best\n");
    // The #148 pin (test_zero_offset_trail_arms_from_the_carried_best,
    // boztilkiserhan 14:15 bar) extended to a sub-tick offset. With the best
    // family Z actually carries (the issuing close 1475.99 < activation
    // 1500.98) the trail is dormant, the bar opens BELOW the activation and
    // never crosses it -> HOLD, as TV does.
    Bar serhan_hold = mk(1475.99, 1491.82, 1475.89, 1486.23);
    const double offsets[] = {0.0, 0.5, 0.9};
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(serhan_hold, PositionSide::LONG,
                                           /*trail_points=*/2213.985, off,
                                           /*entry=*/1478.84, /*best_start=*/1475.99,
                                           /*mintick=*/0.01);
        CHECK(f.filled == false);
    }
    // A carried best past the activation ARMS every zero-tick offset (round
    // 10 family AC: the product trusts the best the placement point hands
    // it; the command layer's restart keeps the #148 peak out): the open
    // through the level 1501.03 is the open print. Two already-armed legs
    // stand at that carried best -- the generic Trail, whose running best
    // lane E14 seeds there, and the sibling kernel STOP the adapter rests
    // for the touch the zero-distance ride cannot take -- and the gapping
    // open is strictly past the best, so the Trail books it first. Either
    // way it is the open print, a point price and not a level.
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(serhan_hold, PositionSide::LONG,
                                           /*trail_points=*/2213.985, off,
                                           /*entry=*/1478.84, /*best_start=*/1501.03,
                                           /*mintick=*/0.01);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 1475.99));
        CHECK(near(f.raw_price, 1475.99));
        CHECK(f.at_bar_open == true);
        // expectation corrected: leg_is_stop -> leg_is_trail, because lane
        // E14 seeds the Trail's running best at the carried level, so the
        // gapping open is already strictly past its zero-distance stop and
        // the Trail books the print the sibling stop used to. The booked
        // price, bar and path position are unchanged.
        CHECK(f.leg_is_trail == true);
        CHECK(f.level_fill == false);
    }
    // Omitted-offset control keeps the durable carried arming: the adapter
    // makes it marketable at the next open (a kernel Market), which books
    // the same open print.
    TrailExitProjection omitted = trail_fill(serhan_hold, PositionSide::LONG,
                                             /*trail_points=*/2213.985, kNaN,
                                             /*entry=*/1478.84, /*best_start=*/1501.03,
                                             /*mintick=*/0.01);
    CHECK(omitted.filled == true);
    CHECK(near(omitted.exit_price, 1475.99));
    CHECK(omitted.leg_is_market == true);
}

// ── engine-level fixtures ─────────────────────────────────────────────

class TrailEngine : public pineforge::source::PineStrategyHost {
public:
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
};

// The family's shape: a POOC market entry (fills at the signal bar's close)
// with strategy.exit(trail_points, trail_offset) issued alongside it and
// re-issued on every bar the position is live.
class PoocAtrTrailProbe : public TrailEngine {
public:
    PoocAtrTrailProbe(bool is_long, double trail_points, double trail_offset,
                      double mintick)
        : is_long_(is_long), trail_points_(trail_points),
          trail_offset_(trail_offset) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = true;
        syminfo_mintick_ = mintick;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("E", is_long_, kNaN, kNaN, /*qty=*/1.0);
        }
        if (bar_index_ == 0 || position_side_ != PositionSide::FLAT) {
            strategy_exit("x", "E", /*limit=*/kNaN, /*stop=*/kNaN,
                          trail_points_, trail_offset_, /*trail_price=*/kNaN);
        }
    }

private:
    bool is_long_;
    double trail_points_;
    double trail_offset_;
};

void test_engine_pooc_xauusd_long_exit_at_open_minus_offset() {
    std::printf("test_engine_pooc_xauusd_long_exit_at_open_minus_offset\n");
    // Bar 0 is the signal bar (POOC close fill @3110.31, no extremes folded
    // into the carried best); bar 1 is the tape bar; bar 2 proves nothing
    // else fires.
    std::vector<Bar> bars = {
        mk(3110.31, 3110.31, 3110.31, 3110.31, 1000),
        mk(3110.40, 3136.775, 3109.24, 3134.46, 2000),
        mk(3134.46, 3135.00, 3133.00, 3134.00, 3000),
    };
    PoocAtrTrailProbe eng(/*is_long=*/true, 15.0, 15.0, /*mintick=*/0.001);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.last_error().empty());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(near(eng.entry_price(0), 3110.31));
        CHECK(near(eng.exit_price(0), 3110.385));   // TV; was 3136.76
        CHECK(eng.exit_bar(0) == 1);
    }
    CHECK(near(eng.position_size(), 0.0));
}

void test_engine_pooc_stock_short_exit_at_open_plus_tick() {
    std::printf("test_engine_pooc_stock_short_exit_at_open_plus_tick\n");
    // POOC close fills at 256.00. The next open 240.00 is below 255.98;
    // its high-first leg crosses open + one tick before visiting low 237.00.
    std::vector<Bar> bars = {
        mk(256.00, 256.00, 256.00, 256.00, 1000),
        mk(240.00, 241.00, 237.00, 239.50, 2000),
        mk(239.50, 240.00, 239.00, 239.75, 3000),
    };
    PoocAtrTrailProbe eng(/*is_long=*/false, 1.7, 1.7, /*mintick=*/0.01);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.last_error().empty());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(near(eng.entry_price(0), 256.00));
        CHECK(near(eng.exit_price(0), 240.01));     // 240.00 + floor(1.7) * 0.01 = 240.01
        CHECK(eng.exit_bar(0) == 1);
    }
    CHECK(near(eng.position_size(), 0.0));
}

// Non-POOC short @100 (entry fills at bar 1's open), trail_points = 3 ticks
// (activation 99.97) armed on bar 1 while the position is live, event bar 2.
class ShortTrailProbe : public TrailEngine {
public:
    explicit ShortTrailProbe(double trail_offset) : trail_offset_(trail_offset) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        syminfo_mintick_ = 0.01;
    }

    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("S", false, kNaN, kNaN, /*qty=*/1.0);
        } else if (bar_index_ == 1) {
            strategy_exit("X", "S", /*limit=*/kNaN, /*stop=*/kNaN,
                          /*trail_points=*/3.0, trail_offset_,
                          /*trail_price=*/kNaN);
        }
    }

private:
    double trail_offset_;
};

struct Outcome {
    int trades;
    double exit_price;
    int exit_bar;
    double position;
};

Outcome run_short(double trail_offset, const std::vector<Bar>& bars) {
    ShortTrailProbe eng(trail_offset);
    eng.run(bars.data(), (int)bars.size());
    Outcome o{eng.trade_count(), kNaN, -1, eng.position_size()};
    if (eng.trade_count() >= 1) {
        o.exit_price = eng.exit_price(0);
        o.exit_bar = eng.exit_bar(0);
    }
    return o;
}

void test_engine_subtick_offsets_match_explicit_zero() {
    std::printf("test_engine_subtick_offsets_match_explicit_zero\n");
    // I. Intrabar cross: high-first event bar (|H-O| = 0.05 < |O-L| = 0.10),
    //    the H->L leg crosses the activation 99.97. Old code with 0.5 / 0.9
    //    armed at the low and filled at 99.90.
    std::vector<Bar> cross = {
        mk(100.00, 100.00, 100.00, 100.00, 1000),
        mk(100.00, 100.00, 100.00, 100.00, 2000),
        mk(100.00, 100.05, 99.90, 99.95, 3000),
    };
    // II. Gapped open: the event bar opens at 99.90, past the activation ->
    //     fill at the open. Old code with 0.5 / 0.9 filled at the low 99.80.
    std::vector<Bar> gapped = {
        mk(100.00, 100.00, 100.00, 100.00, 1000),
        mk(100.00, 100.00, 100.00, 100.00, 2000),
        mk(99.90, 99.95, 99.80, 99.85, 3000),
    };
    struct Scenario { const std::vector<Bar>* bars; double expected; };
    const Scenario scenarios[] = {{&cross, 99.97}, {&gapped, 99.90}};
    for (const Scenario& sc : scenarios) {
        Outcome zero = run_short(0.0, *sc.bars);
        CHECK(zero.trades == 1);
        CHECK(near(zero.exit_price, sc.expected));
        CHECK(zero.exit_bar == 2);
        CHECK(near(zero.position, 0.0));
        const double subtick[] = {0.5, 0.9};
        for (double off : subtick) {
            Outcome o = run_short(off, *sc.bars);
            CHECK(o.trades == zero.trades);
            CHECK(near(o.exit_price, zero.exit_price));
            CHECK(o.exit_bar == zero.exit_bar);
            CHECK(near(o.position, zero.position));
        }
    }
}

void test_engine_subtick_offset_arms_from_the_placement_close() {
    std::printf("test_engine_subtick_offset_arms_from_the_placement_close\n");
    // The exit is issued on bar 1 at its close 99.60 (the running extreme
    // restarts there — round 9 family Z), already past the 99.97 activation
    // for a short; bar 2 opens ABOVE that level and never trades down to it.
    // Every zero-tick offset (0 / 0.5 / 0.9) is armed at the placement close
    // and the open gaps through its level -> the open print 100.20, exactly
    // like the omitted offset. A dormant one-shot would incorrectly HOLD
    // because the event bar never reaches activation after its open.
    std::vector<Bar> bars = {
        mk(100.00, 100.00, 100.00, 100.00, 1000),
        mk(100.00, 100.00, 99.50, 99.60, 2000),
        mk(100.20, 100.30, 100.05, 100.10, 3000),
        mk(100.10, 100.15, 100.00, 100.05, 4000),
    };
    const double one_shot[] = {0.0, 0.5, 0.9};
    for (double off : one_shot) {
        Outcome o = run_short(off, bars);
        CHECK(o.trades == 1);
        CHECK(near(o.exit_price, 100.20));
        CHECK(o.exit_bar == 2);
        CHECK(near(o.position, 0.0));
    }
    Outcome omitted = run_short(kNaN, bars);
    CHECK(omitted.trades == 1);
    CHECK(near(omitted.exit_price, 100.20));
    CHECK(omitted.exit_bar == 2);
    CHECK(near(omitted.position, 0.0));
}

}  // namespace

int main() {
    std::printf("=== test_trail_open_arm_subtick_offset ===\n");

    test_open_arms_trail_xauusd_long();
    test_open_arms_trail_stock_short();
    test_open_arms_trail_ford_daily_long();
    test_open_is_a_new_best_for_an_armed_trail();
    test_open_below_activation_does_not_arm();

    test_subtick_offset_fills_at_activation_eurusd_short();
    test_subtick_offset_fills_at_activation_long();
    test_subtick_offset_gapped_open_arms_and_rides();
    test_whole_tick_offsets_keep_floored_trailing_distance();
    test_subtick_offset_arms_from_the_carried_best();

    test_engine_pooc_xauusd_long_exit_at_open_minus_offset();
    test_engine_pooc_stock_short_exit_at_open_plus_tick();
    test_engine_subtick_offsets_match_explicit_zero();
    test_engine_subtick_offset_arms_from_the_placement_close();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
