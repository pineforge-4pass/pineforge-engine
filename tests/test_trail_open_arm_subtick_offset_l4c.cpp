/*
 * test_trail_open_arm_subtick_offset.cpp — two trailing-exit rules pinned
 * from TradingView tapes (winthetrade-ema-9-vwap-strategy-with-atr-trailing-
 * stop family: strategy.exit("x", trail_points = atr*2, trail_offset = atr*2),
 * a price-unit ATR passed as TICKS; 7 lanes, entry/exit timestamps exact,
 * exit PRICE wrong on the same bar).
 *
 * (B) The bar's OPEN is the first price a resting trail observes. A trail
 *     whose activation the open already sits past arms AT the open with
 *     best = open, so an adverse-first leg fills at open -/+ offset. The
 *     engine only folded prices in at the END of each path segment, stayed
 *     dormant through the retrace, armed at the favourable extreme and
 *     filled at extreme -/+ offset.
 *       OANDA:XAUUSD 15m long 2025-04-04 10:45Z @3110.31 (POOC close fill);
 *         11:00Z bar O 3110.40 H 3136.775 L 3109.24 C 3134.46, 15t/15t,
 *         mintick 0.001: TV 3110.385 (open - 0.015), engine 3136.76.
 *       NASDAQ:AAPL 15m short 2025-04-02 18:45Z @222.93; 04-03 13:30Z bar
 *         O 205.54 L 202.52: TV 205.55 (open + 1t), engine 202.53 (low + 1t).
 *       NYSE:F 1D long 2025-04-21 @9.47; 04-22 O 9.55 H 9.72: TV 9.54,
 *         engine 9.71.
 *
 * (A) A trail_offset whose floor is ZERO ticks (any value in [0, 1)) is
 *     TV's explicit-zero trail: an activation first reached intrabar is the
 *     one-shot fill AT the activation; armed (at the placement close, by an
 *     open past the activation, or by a path extreme) it is a zero-distance
 *     trailing stop on the raw running best (round 10 family AC,
 *     test_zero_offset_trail_rides). `lab tv` pin on OANDA:EURUSD 15m
 *     2025-04-01 -> 05-01: strategy.exit("x", "L", trail_points=3,
 *     trail_offset=0 / 0.5 / 0.9) produce byte-identical tapes (190 rows,
 *     sha256 36aa80ac...).
 *       EURUSD 15m short 2025-03-31 03:45Z @1.08330, atr*2 ~ 0.0006 ticks
 *         (activation ceil -> 1t = 1.08329, offset floor -> 0); exit bar
 *         O 1.08330 L 1.08314: TV 1.08329 (activation), engine 1.08314 (low)
 *         — the old finite-zero distance armed at the low and filled there
 *         although the activation was first reached intrabar.
 *
 * Resolver-level pins go straight through resolve_exit_path_fill (the
 * runtime-private header, as test_path_resolve_extra.cpp does); engine-level
 * pins run BacktestEngine end to end, including the POOC close-fill entry
 * whose carried best is the entry price itself.
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/engine_internal.hpp"

using namespace pineforge;
using namespace pineforge::internal;

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

// Resolver call for a lone trailing exit (no stop / limit legs) resting on
// a NON-entry bar from the open, in the plain (non-magnifier) path.
ExitPathFill trail_fill(const Bar& bar, PositionSide side,
                        double trail_points, double trail_offset,
                        double entry, double best_start, double mintick) {
    return resolve_exit_path_fill(
        bar, side, /*stop=*/kNaN, /*limit=*/kNaN,
        trail_points, /*trail_price=*/kNaN, trail_offset, entry,
        best_start, /*is_entry_bar=*/false, /*magnifier_active=*/false,
        mintick);
}

// ── (B) resolver: the open arms the trail ─────────────────────────────

void test_open_arms_trail_xauusd_long() {
    std::printf("test_open_arms_trail_xauusd_long\n");
    // Carried best = entry (POOC close fill, no entry-bar extremes folded).
    // activation = 3110.31 + 15*0.001 = 3110.325 <= open 3110.40 -> armed at
    // the open, best = open. |O-L| = 1.16 < |H-O| = 26.375 -> low-first path
    // O -> L -> H -> C; the O->L leg crosses open - 0.015 = 3110.385.
    Bar xau = mk(3110.40, 3136.775, 3109.24, 3134.46);
    ExitPathFill f = trail_fill(xau, PositionSide::LONG,
                                /*trail_points=*/15.0, /*trail_offset=*/15.0,
                                /*entry=*/3110.31, /*best_start=*/3110.31,
                                /*mintick=*/0.001);
    CHECK(f.should_fill == true);
    CHECK(near(f.fill_price, 3110.385));
    CHECK(f.is_trail == true);
    CHECK(f.is_limit == false);
    CHECK(f.at_bar_open == false);
    // On the O->L leg: (3110.385 - 3110.40) / (3109.24 - 3110.40).
    CHECK(near(f.path_position, 0.015 / 1.16, 1e-6));
}

void test_open_arms_trail_aapl_short() {
    std::printf("test_open_arms_trail_aapl_short\n");
    // atr*2 ~ 1.7 ticks: activation ceil -> 2t = 222.91 >= open 205.54 ->
    // armed at the open; offset floor -> 1t = 0.01. |H-O| = 1.46 <
    // |O-L| = 3.02 -> high-first path; the O->H leg crosses open + 0.01.
    Bar aapl = mk(205.54, 207.00, 202.52, 204.00);
    ExitPathFill f = trail_fill(aapl, PositionSide::SHORT,
                                /*trail_points=*/1.7, /*trail_offset=*/1.7,
                                /*entry=*/222.93, /*best_start=*/222.93,
                                /*mintick=*/0.01);
    CHECK(f.should_fill == true);
    CHECK(near(f.fill_price, 205.55));
    CHECK(f.is_trail == true);
    CHECK(f.at_bar_open == false);
}

void test_open_arms_trail_ford_daily_long() {
    std::printf("test_open_arms_trail_ford_daily_long\n");
    // activation = 9.47 + 2*0.01 = 9.49 <= open 9.55; offset 1t. Adverse-first
    // path (|O-L| = 0.05 < |H-O| = 0.17) crosses open - 0.01 = 9.54.
    Bar ford = mk(9.55, 9.72, 9.50, 9.70);
    ExitPathFill f = trail_fill(ford, PositionSide::LONG,
                                /*trail_points=*/1.5, /*trail_offset=*/1.5,
                                /*entry=*/9.47, /*best_start=*/9.47,
                                /*mintick=*/0.01);
    CHECK(f.should_fill == true);
    CHECK(near(f.fill_price, 9.54));
}

void test_open_is_a_new_best_for_an_armed_trail() {
    std::printf("test_open_is_a_new_best_for_an_armed_trail\n");
    // Consequence of the same rule (not a separately tape-pinned value): a
    // trail already armed from the carried best (102 >= activation 101) sees
    // a gap-up open 103 as its new best, so the adverse-first leg fills at
    // 103 - 0.5 = 102.5 rather than trailing the stale 102 - 0.5 = 101.5
    // (which this bar never reaches — the old walk produced NO fill here).
    Bar gap_up = mk(103.0, 104.0, 102.4, 103.8);
    ExitPathFill f = trail_fill(gap_up, PositionSide::LONG,
                                /*trail_points=*/100, /*trail_offset=*/50,
                                /*entry=*/100.0, /*best_start=*/102.0,
                                /*mintick=*/0.01);
    CHECK(f.should_fill == true);
    CHECK(near(f.fill_price, 102.5));
}

void test_open_below_activation_does_not_arm() {
    std::printf("test_open_below_activation_does_not_arm\n");
    // Control: open 100.5 < activation 101 -> nothing changes; the trail
    // still arms at the high and retraces from it (the established
    // test_resolve_exit_trail_fills pin: fill 101.5).
    Bar bar = mk(100.5, 102, 100, 100.2);
    ExitPathFill f = trail_fill(bar, PositionSide::LONG,
                                /*trail_points=*/100, /*trail_offset=*/50,
                                /*entry=*/100.0, /*best_start=*/kNaN,
                                /*mintick=*/0.01);
    CHECK(f.should_fill == true);
    CHECK(near(f.fill_price, 101.5));
}

// ── (A) resolver: sub-tick offset == explicit zero ────────────────────

void test_subtick_offset_fills_at_activation_eurusd_short() {
    std::printf("test_subtick_offset_fills_at_activation_eurusd_short\n");
    // trail_points 0.6 -> ceil 1t -> activation 1.08329; offsets 0 / 0.5 /
    // 0.9 / 0.6 all floor to 0 ticks -> the one-shot rule fills AT 1.08329
    // on the O->L leg (|H-O| = 0.0002 >= |O-L| = 0.00016 -> low-first path).
    // The old finite-zero distance armed at the low and filled at 1.08314.
    Bar eur = mk(1.08330, 1.08350, 1.08314, 1.08320);
    const double offsets[] = {0.0, 0.5, 0.9, 0.6};
    for (double off : offsets) {
        ExitPathFill f = trail_fill(eur, PositionSide::SHORT,
                                    /*trail_points=*/0.6, off,
                                    /*entry=*/1.08330, /*best_start=*/1.08330,
                                    /*mintick=*/0.00001);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1.08329));
        CHECK(f.is_trail == true);
        CHECK(f.at_bar_open == false);
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
        ExitPathFill f = trail_fill(bar, PositionSide::LONG,
                                    /*trail_points=*/3.0, off,
                                    /*entry=*/100.0, /*best_start=*/100.0,
                                    /*mintick=*/0.01);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 100.03));
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
        ExitPathFill f = trail_fill(gap, PositionSide::SHORT,
                                    /*trail_points=*/0.6, off,
                                    /*entry=*/1.08330, /*best_start=*/1.08330,
                                    /*mintick=*/0.00001);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1.08320));
        CHECK(f.at_bar_open == true);
        CHECK(near(f.path_position, 0.0));
    }
    // High-first twin, same on-grid open: same fill. The path never gets to
    // matter. (Before pins4 this fixture asserted a ride to the 1.08305 low
    // on the low-first bar and a level fill at the open on the high-first
    // one; the ride is what a SUB-TICK open does — the twin below — and the
    // 11 on-grid tapes in test_zero_offset_trail_rides refute it here.)
    Bar gap_hf = mk(1.08320, 1.08330, 1.08300, 1.08310);
    for (double off : offsets) {
        ExitPathFill f = trail_fill(gap_hf, PositionSide::SHORT,
                                    /*trail_points=*/0.6, off,
                                    /*entry=*/1.08330, /*best_start=*/1.08330,
                                    /*mintick=*/0.00001);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1.08320));
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
    // it. NYSE:F g2-0321-S-tp5 (open 9.915 -> TV fills the 9.86 low) and
    // s-dsub-0313-tp2 (9.575 -> the 9.51 low) are the tapes this stands for.
    Bar gap_sub = mk(1.083205, 1.08340, 1.08305, 1.08330);
    for (double off : offsets) {
        ExitPathFill f = trail_fill(gap_sub, PositionSide::SHORT,
                                    /*trail_points=*/0.6, off,
                                    /*entry=*/1.08330, /*best_start=*/1.08330,
                                    /*mintick=*/0.00001);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1.08305));
        CHECK(f.is_trail == true);
        CHECK(f.at_bar_open == false);
        // The start of the L->H leg (segment 2 of O->L->H->C).
        CHECK(near(f.path_position, 1.0));
    }
    // 1.083215 / 0.00001 = 108321.5 prints 1.08322 = the ceiled level: the
    // print sits AT the short's stop and the exit fills at the open, a level
    // fill the consumer ceils to 1.08322, whatever the path (NASDAQ:AAPL
    // 04-29 13:30Z short open 208.955 -> TV 208.96, 02-04 227.125 -> 227.13;
    // NYSE:F 03-06 14:30Z 9.515 -> 9.52: round 10 family AC pins4 / pins5).
    Bar gap_sub_at = mk(1.083215, 1.08340, 1.08305, 1.08330);
    for (double off : offsets) {
        ExitPathFill f = trail_fill(gap_sub_at, PositionSide::SHORT,
                                    /*trail_points=*/0.6, off,
                                    /*entry=*/1.08330, /*best_start=*/1.08330,
                                    /*mintick=*/0.00001);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1.083215));
        CHECK(f.at_bar_open == true);
        CHECK(f.open_is_trail_level == true);
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
        ExitPathFill f = trail_fill(eur, PositionSide::SHORT,
                                    /*trail_points=*/0.6, off,
                                    /*entry=*/1.08330, /*best_start=*/1.08330,
                                    /*mintick=*/0.00001);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1.08315));
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
        ExitPathFill f = trail_fill(serhan_hold, PositionSide::LONG,
                                    /*trail_points=*/2213.985, off,
                                    /*entry=*/1478.84, /*best_start=*/1475.99,
                                    /*mintick=*/0.01);
        CHECK(f.should_fill == false);
    }
    // A carried best past the activation ARMS every zero-tick offset (round
    // 10 family AC: the resolver trusts the best it is handed; the command
    // layer's restart keeps the #148 peak out): the open through the level
    // 1501.03 is the open print.
    for (double off : offsets) {
        ExitPathFill f = trail_fill(serhan_hold, PositionSide::LONG,
                                    /*trail_points=*/2213.985, off,
                                    /*entry=*/1478.84, /*best_start=*/1501.03,
                                    /*mintick=*/0.01);
        CHECK(f.should_fill == true);
        CHECK(near(f.fill_price, 1475.99));
        CHECK(f.at_bar_open == true);
        CHECK(f.open_is_trail_level == false);
    }
    // Omitted-offset control keeps the durable carried arming (gap-fill at
    // the open), exactly as pinned in test_path_resolve_extra.
    ExitPathFill omitted = trail_fill(serhan_hold, PositionSide::LONG,
                                      /*trail_points=*/2213.985, kNaN,
                                      /*entry=*/1478.84, /*best_start=*/1501.03,
                                      /*mintick=*/0.01);
    CHECK(omitted.should_fill == true);
    CHECK(near(omitted.fill_price, 1475.99));
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

void test_engine_pooc_aapl_short_exit_at_open_plus_tick() {
    std::printf("test_engine_pooc_aapl_short_exit_at_open_plus_tick\n");
    std::vector<Bar> bars = {
        mk(222.93, 222.93, 222.93, 222.93, 1000),
        mk(205.54, 207.00, 202.52, 204.00, 2000),
        mk(204.00, 204.50, 203.50, 204.20, 3000),
    };
    PoocAtrTrailProbe eng(/*is_long=*/false, 1.7, 1.7, /*mintick=*/0.01);
    eng.run(bars.data(), (int)bars.size());

    CHECK(eng.last_error().empty());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(near(eng.entry_price(0), 222.93));
        CHECK(near(eng.exit_price(0), 205.55));     // TV; was 202.53
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
    // like the omitted offset (round 10 family AC, test_zero_offset_trail_rides
    // f-gapdown-0404-1600-tp4 / 0423-1345-tp7b; test_margin_call_trail_exit_
    // chronology fixture D). The old one-shot reading HELD here.
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
    test_open_arms_trail_aapl_short();
    test_open_arms_trail_ford_daily_long();
    test_open_is_a_new_best_for_an_armed_trail();
    test_open_below_activation_does_not_arm();

    test_subtick_offset_fills_at_activation_eurusd_short();
    test_subtick_offset_fills_at_activation_long();
    test_subtick_offset_gapped_open_arms_and_rides();
    test_whole_tick_offsets_keep_floored_trailing_distance();
    test_subtick_offset_arms_from_the_carried_best();

    test_engine_pooc_xauusd_long_exit_at_open_minus_offset();
    test_engine_pooc_aapl_short_exit_at_open_plus_tick();
    test_engine_subtick_offsets_match_explicit_zero();
    test_engine_subtick_offset_arms_from_the_placement_close();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return (tests_failed > 0) ? 1 : 0;
}
