/*
 * test_path_resolve_extra.cpp — pin the path-resolution helpers in
 * src/engine_path_resolve.cpp that the engine-driven bracket tests
 * (test_exit_path_segment_tiebreak.cpp) leave uncovered.
 *
 * These helpers live in pineforge::internal and are declared in the
 * RUNTIME-PRIVATE header src/engine_internal.hpp. libpineforge is a STATIC
 * archive, so the symbols resolve at link time even though they are hidden
 * from any .so export table. We call them directly to drive the exact
 * branch logic, AND through the public free functions
 * resolve_exit_path_fill / exit_order_earliest_path_metric_no_trail to
 * reach the anonymous-namespace trail/gap/entry-bar helpers that are not
 * individually addressable.
 *
 * Every expected value below is a closed-form function of the OHLC
 * waypoints with mintick = 0.01 and lands exactly on the tick grid:
 *
 *   - bar_path_uses_high_first: high-first iff |H-O| < |O-L| (ties low-first)
 *   - high-first path: O -> H -> L -> C ;  low-first: O -> L -> H -> C
 *   - within a segment, the FIRST level crossed (smaller parametric t) wins
 *   - an exit stop/limit that GAPS past the bar open fills at the open
 *   - trail arms once the running best crosses the activation level; with no
 *     offset it exits AT the activation level, with an offset it trails best±off
 *   - on the entry bar a no-trail exit on the wrong side of entry is blocked
 *
 * All asserts are real Pine-correct return values derived by instrumenting
 * the engine, not tautologies.
 */

#include <cmath>
#include <cstdio>
#include <limits>
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

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kMintick = 0.01;

Bar mk(double o, double h, double l, double c) {
    Bar b{};
    b.open = o; b.high = h; b.low = l; b.close = c;
    b.volume = 1000.0; b.timestamp = 0;
    return b;
}

PendingOrder mk_raw(double stop, double limit, bool is_long) {
    PendingOrder o{};
    o.type = OrderType::RAW_ORDER;
    o.is_long = is_long;
    o.stop_price = stop;
    o.limit_price = limit;
    o.trail_points = kNaN;
    o.trail_offset = kNaN;
    o.qty = kNaN;
    o.qty_type = -1;
    o.qty_percent = 100.0;
    o.oca_type = 0;
    o.created_bar = 0;
    return o;
}

PendingOrder mk_exit(double stop, double limit, double trail_points,
                     double trail_offset) {
    PendingOrder o{};
    o.type = OrderType::EXIT;
    o.is_long = false;
    o.stop_price = stop;
    o.limit_price = limit;
    o.trail_points = trail_points;
    o.trail_offset = trail_offset;
    o.qty = kNaN;
    o.qty_type = -1;
    o.qty_percent = 100.0;
    o.oca_type = 0;
    o.created_bar = 0;
    return o;
}

}  // namespace

// ── price_path_priority: -1 stop first, +1 limit first, 0 neither ──
//
// Bar (100, 101, 98, 100): |H-O|=1 < |O-L|=2 -> high-first path
//   O(100) -> H(101) -> L(98) -> C(100).
// The H->L segment descends 101 -> 98 and contains both levels in the
// scenarios below; parametric t along that segment decides who fires first.
static void test_price_path_priority_branches() {
    std::printf("test_price_path_priority_branches\n");
    Bar hi_first = mk(100, 101, 98, 100);
    CHECK(bar_path_uses_high_first(hi_first) == true);

    // (a) stop nearer the open ALONG the H->L leg:
    //   t_stop=(100-101)/(98-101)=1/3, t_limit=(99-101)/(98-101)=2/3 -> stop first.
    CHECK(price_path_priority(hi_first, /*stop=*/100, /*limit=*/99) == -1);

    // (b) limit nearer the open along the same leg:
    //   t_stop=2/3, t_limit=1/3 -> limit first.
    CHECK(price_path_priority(hi_first, /*stop=*/99, /*limit=*/100) == 1);

    // (g) equal levels in a non-degenerate segment -> exact t tie -> -1.
    CHECK(price_path_priority(hi_first, /*stop=*/99.5, /*limit=*/99.5) == -1);

    // (d) only the stop falls in any segment -> -1.
    CHECK(price_path_priority(hi_first, /*stop=*/99.5, /*limit=*/kNaN) == -1);

    // (e) only the limit falls in any segment -> +1.
    CHECK(price_path_priority(hi_first, /*stop=*/kNaN, /*limit=*/99.5) == 1);

    // (f) neither level inside the bar's range -> 0.
    CHECK(price_path_priority(hi_first, /*stop=*/50, /*limit=*/200) == 0);

    // (c) degenerate segment: bar (100,100,99,99.5) has |H-O|=0 < |O-L|=1
    //   -> high-first path O(100) -> H(100) -> L(99) -> C(99.5). The first leg
    //   O->H is FLAT at 100; both levels==100 land in it with denom~0 -> -1.
    Bar degenerate = mk(100, 100, 99, 99.5);
    CHECK(bar_path_uses_high_first(degenerate) == true);
    CHECK(price_path_priority(degenerate, /*stop=*/100, /*limit=*/100) == -1);
}

// ── exit_order_touch_position: gap-through-open arms fill at path pos 0 ──
//
// When bar.open already sits at/past the exit level in the firing direction,
// the order fills at the open (path position 0) — one arm per (side, kind).
static void test_exit_order_touch_gap_arms() {
    std::printf("test_exit_order_touch_gap_arms\n");
    double pos = -1.0;

    // LONG position, pure-stop exit: open=96 gaps below stop=98 -> pos 0.
    Bar long_stop_gap = mk(96, 99, 95, 97);
    PendingOrder ls = mk_raw(/*stop=*/98, /*limit=*/kNaN, /*is_long=*/false);
    CHECK(exit_order_touch_position(long_stop_gap, ls, PositionSide::LONG, &pos));
    CHECK(near(pos, 0.0));

    // LONG position, pure-limit exit: open=103 gaps above limit=102 -> pos 0.
    Bar long_limit_gap = mk(103, 104, 102, 103);
    PendingOrder ll = mk_raw(/*stop=*/kNaN, /*limit=*/102, /*is_long=*/false);
    CHECK(exit_order_touch_position(long_limit_gap, ll, PositionSide::LONG, &pos));
    CHECK(near(pos, 0.0));

    // SHORT position, pure-stop exit: open=104 gaps above stop=102 -> pos 0.
    Bar short_stop_gap = mk(104, 105, 103, 104);
    PendingOrder ss = mk_raw(/*stop=*/102, /*limit=*/kNaN, /*is_long=*/true);
    CHECK(exit_order_touch_position(short_stop_gap, ss, PositionSide::SHORT, &pos));
    CHECK(near(pos, 0.0));

    // SHORT position, pure-limit exit: open=97 gaps below limit=98 -> pos 0.
    Bar short_limit_gap = mk(97, 98, 96, 97);
    PendingOrder sl = mk_raw(/*stop=*/kNaN, /*limit=*/98, /*is_long=*/true);
    CHECK(exit_order_touch_position(short_limit_gap, sl, PositionSide::SHORT, &pos));
    CHECK(near(pos, 0.0));

    // Non-gap LONG stop: open=100 above stop=98, low=97 reaches it later.
    //   high-first (|1|<|3|) path 100->101->97->99; stop 98 on the 101->97 leg
    //   at pos 1 + (98-101)/(97-101) = 1 + 0.75 = 1.75.
    Bar non_gap = mk(100, 101, 97, 99);
    PendingOrder ng = mk_raw(/*stop=*/98, /*limit=*/kNaN, /*is_long=*/false);
    CHECK(exit_order_touch_position(non_gap, ng, PositionSide::LONG, &pos));
    CHECK(near(pos, 1.75));

    // Pure-na / dual-priced orders are rejected (has_stop == has_limit).
    PendingOrder both = mk_raw(/*stop=*/98, /*limit=*/102, /*is_long=*/false);
    CHECK(exit_order_touch_position(non_gap, both, PositionSide::LONG, &pos) == false);
    // FLAT position is never an exit context.
    CHECK(exit_order_touch_position(non_gap, ng, PositionSide::FLAT, &pos) == false);
}

// ── path_cross_kind_priority: STOP(0) < TRAIL(1) < LIMIT(2) ──
//
// When several levels cross at the same path position, collect_cross_events
// orders them by this priority so a stop beats a co-located trail beats a
// co-located limit. Exercised both directly and through the sort.
static void test_path_cross_kind_priority_order() {
    std::printf("test_path_cross_kind_priority_order\n");
    CHECK(path_cross_kind_priority(PathCrossKind::STOP) == 0);
    CHECK(path_cross_kind_priority(PathCrossKind::TRAIL) == 1);
    CHECK(path_cross_kind_priority(PathCrossKind::LIMIT) == 2);

    // All three cross at the midpoint of a 100->110 leg (pos 0.5). The sort
    // comparator must emit them STOP, TRAIL, LIMIT.
    CrossEventList ev =
        collect_cross_events(100, 110, /*stop=*/105, /*limit=*/105, /*trail=*/105);
    CHECK(ev.n == 3);
    CHECK(ev.ev[0].kind == PathCrossKind::STOP);
    CHECK(ev.ev[1].kind == PathCrossKind::TRAIL);
    CHECK(ev.ev[2].kind == PathCrossKind::LIMIT);
    CHECK(near(ev.ev[0].path_pos, 0.5));
    CHECK(near(ev.ev[2].path_pos, 0.5));

    // A lone trail level still appends (kind TRAIL) at its interpolated pos.
    CrossEventList trail_only =
        collect_cross_events(100, 110, kNaN, kNaN, /*trail=*/107);
    CHECK(trail_only.n == 1);
    CHECK(trail_only.ev[0].kind == PathCrossKind::TRAIL);
    CHECK(near(trail_only.ev[0].path_pos, 0.7));

    // Levels outside the leg are not appended.
    CrossEventList none =
        collect_cross_events(100, 110, /*stop=*/120, /*limit=*/90, kNaN);
    CHECK(none.n == 0);
}

// ── resolve_exit_path_fill: trailing-stop activation + fill levels ──
static void test_resolve_exit_trail_fills() {
    std::printf("test_resolve_exit_trail_fills\n");

    // FLAT short-circuits to no fill regardless of levels.
    Bar flat_bar = mk(100, 102, 98, 100);
    ExitPathFill flat = resolve_exit_path_fill(
        flat_bar, PositionSide::FLAT, /*stop=*/98, /*limit=*/102,
        /*trail_points=*/kNaN, /*trail_price=*/kNaN, /*trail_offset=*/kNaN, /*entry=*/100,
        /*best_start=*/kNaN, /*is_entry_bar=*/false, /*magnifier=*/false,
        kMintick);
    CHECK(flat.should_fill == false);

    // LONG trail WITH offset, arming intrabar (update_exit_trail_state rising,
    // active_exit_trail_level = best - offset).
    //   entry=100, trail_points=100 ticks -> activation = 100 + 100*0.01 = 101.
    //   trail_offset = 50 ticks -> 0.50 price.
    //   Bar (100.5, 102, 100, 100.2): |H-O|=1.5 NOT < |O-L|=0.5 -> LOW-first
    //   path O(100.5) -> L(100) -> H(102) -> C(100.2).
    //     leg L->H rises to 102 -> best=102 >= 101 -> trail arms.
    //     leg H->C falls 102->100.2; trail level = 102 - 0.5 = 101.5, crossed -> fill@101.5.
    Bar trail_long = mk(100.5, 102, 100, 100.2);
    ExitPathFill fl = resolve_exit_path_fill(
        trail_long, PositionSide::LONG, /*stop=*/kNaN, /*limit=*/kNaN,
        /*trail_points=*/100, /*trail_price=*/kNaN, /*trail_offset=*/50, /*entry=*/100,
        /*best_start=*/kNaN, /*is_entry_bar=*/false, /*magnifier=*/false,
        kMintick);
    CHECK(fl.should_fill == true);
    CHECK(near(fl.fill_price, 101.5));

    // LONG trail with NO offset -> exits AT the activation level itself
    //   (active_exit_trail_level returns activation_level; the limit-leg of
    //   select_exit_segment_levels arms trail_level=activation when not yet active).
    //   activation = 101 is crossed on the rising L->H leg -> fill@101.
    ExitPathFill fl_nooff = resolve_exit_path_fill(
        trail_long, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/100, /*trail_price=*/kNaN, /*trail_offset=*/kNaN, /*entry=*/100,
        /*best_start=*/kNaN, false, false, kMintick);
    CHECK(fl_nooff.should_fill == true);
    CHECK(near(fl_nooff.fill_price, 101.0));

    // SHORT trail WITH offset, arming on a FALLING leg (update_exit_trail_state
    // short branch, best tracks the low).
    //   entry=100, trail_points=100 -> activation = 100 - 1 = 99. offset 50t = 0.5.
    //   Bar (99.5, 101.5, 98, 99.8): |H-O|=2 >= |O-L|=1.5 -> low-first path
    //     99.5 -> 98 -> 101.5 -> 99.8.
    //     leg O->L falls to 98 <= 99 -> trail arms, best=98.
    //     leg L->H rises 98->101.5; trail level = best + offset = 98 + 0.5 = 98.5 -> fill@98.5.
    Bar trail_short = mk(99.5, 101.5, 98, 99.8);
    ExitPathFill fs = resolve_exit_path_fill(
        trail_short, PositionSide::SHORT, kNaN, kNaN,
        /*trail_points=*/100, /*trail_price=*/kNaN, /*trail_offset=*/50, /*entry=*/100,
        /*best_start=*/kNaN, false, false, kMintick);
    CHECK(fs.should_fill == true);
    CHECK(near(fs.fill_price, 98.5));

    // Explicit trail_offset=0 follows the same activation-only path as an
    // omitted offset. These two bars are the first short/long trailing exits
    // from the Boz WMA+ADX strategy. Before the fix, finite zero was treated
    // as a normal trailing distance: the resolver armed at the favorable
    // extreme, then retraced zero ticks and filled at that extreme instead of
    // at the activation crossing.
    //
    // SHORT, dynamically re-issued exit:
    //   entry=1861.49, latest trail_points=1872.14*0.008/0.01=1497.712
    //   -> ceil(1497.712)=1498 ticks -> activation=1846.51.
    //   High-first path 1872.14 -> 1875.00 -> 1843.97 -> 1849.27 must fill
    //   at 1846.51, not the favorable low 1843.97.
    Bar boz_short = mk(1872.14, 1875.00, 1843.97, 1849.27);
    ExitPathFill boz_short_zero = resolve_exit_path_fill(
        boz_short, PositionSide::SHORT, kNaN, kNaN,
        /*trail_points=*/1497.712, /*trail_price=*/kNaN,
        /*trail_offset=*/0.0, /*entry=*/1861.49,
        /*best_start=*/1858.80, false, false, kMintick);
    CHECK(boz_short_zero.should_fill == true);
    CHECK(near(boz_short_zero.fill_price, 1846.51));

    // LONG, first exit snapshot:
    //   entry=1903.31, trail_points=1910*0.008/0.01=1528 ticks
    //   -> activation=1918.59.
    //   Low-first path 1910.00 -> 1907.98 -> 1920.58 -> 1919.43 must fill
    //   at 1918.59, not the favorable high 1920.58.
    Bar boz_long = mk(1910.00, 1920.58, 1907.98, 1919.43);
    ExitPathFill boz_long_zero = resolve_exit_path_fill(
        boz_long, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/1528.0, /*trail_price=*/kNaN,
        /*trail_offset=*/0.0, /*entry=*/1903.31,
        /*best_start=*/1910.00, false, false, kMintick);
    CHECK(boz_long_zero.should_fill == true);
    CHECK(near(boz_long_zero.fill_price, 1918.59));

    // LONG trail no-offset, ALREADY armed via best_start, bar opens past the
    // activation level -> gap-fill at the open (exits-at-activation gap arm).
    //   activation=101; best_start=101.5 (>=activation so armed); open=102>=101.
    Bar gap_nooff = mk(102, 103, 101, 102);
    ExitPathFill g_nooff = resolve_exit_path_fill(
        gap_nooff, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/100, /*trail_price=*/kNaN, /*trail_offset=*/kNaN, /*entry=*/100,
        /*best_start=*/101.5, false, false, kMintick);
    CHECK(g_nooff.should_fill == true);
    CHECK(near(g_nooff.fill_price, 102.0));

    // LONG trail WITH offset, already armed via best_start, bar opens at/under
    // the trail level -> gap-fill at the open (active-trail gap arm, best-off).
    //   best_start=102, offset 50t=0.5 -> trail level=101.5; open=101<=101.5.
    Bar gap_off = mk(101, 101.5, 100, 100.5);
    ExitPathFill g_off = resolve_exit_path_fill(
        gap_off, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/100, /*trail_price=*/kNaN, /*trail_offset=*/50, /*entry=*/100,
        /*best_start=*/102, false, false, kMintick);
    CHECK(g_off.should_fill == true);
    CHECK(near(g_off.fill_price, 101.0));

    // SHORT trail armed by ABSOLUTE trail_price (not trail_points). Pine's
    // strategy.exit(trail_price=..., trail_offset=...) arms the trail at the
    // given absolute price level rather than an entry-relative tick offset.
    //   activation = trail_price = 99 (absolute); offset 50t = 0.5.
    //   Bar (99.5, 101.5, 98, 99.8): low-first path 99.5 -> 98 -> 101.5 -> 99.8.
    //     leg O->L falls to 98 <= 99 -> trail arms, best=98.
    //     leg L->H rises 98->101.5; trail level = best + offset = 98.5 -> fill@98.5.
    ExitPathFill fs_tp = resolve_exit_path_fill(
        trail_short, PositionSide::SHORT, kNaN, kNaN,
        /*trail_points=*/kNaN, /*trail_price=*/99, /*trail_offset=*/50, /*entry=*/100,
        /*best_start=*/kNaN, false, false, kMintick);
    CHECK(fs_tp.should_fill == true);
    CHECK(near(fs_tp.fill_price, 98.5));

    // LONG trail armed by absolute trail_price with no offset -> exits AT the
    // activation price itself when the path crosses up through it.
    //   activation = trail_price = 101; rising L->H leg crosses 101 -> fill@101.
    ExitPathFill fl_tp = resolve_exit_path_fill(
        trail_long, PositionSide::LONG, kNaN, kNaN,
        /*trail_points=*/kNaN, /*trail_price=*/101, /*trail_offset=*/kNaN, /*entry=*/100,
        /*best_start=*/kNaN, false, false, kMintick);
    CHECK(fl_tp.should_fill == true);
    CHECK(near(fl_tp.fill_price, 101.0));
}

// ── exit_order_earliest_path_metric_no_trail: entry-bar wrong-side block ──
//
// On the entry bar a no-trail EXIT whose stop/limit lies on the wrong side of
// entry would have fired before the position existed -> blocked (+inf metric).
// Off the entry bar, or on the correct side, it returns a finite coordinate.
static void test_entry_bar_blocks_no_trail_exit() {
    std::printf("test_entry_bar_blocks_no_trail_exit\n");
    Bar wide = mk(100, 105, 95, 100);  // spans both 102 and 98
    const double inf = std::numeric_limits<double>::infinity();

    // LONG entry@100, stop ABOVE entry -> wrong side -> blocked.
    PendingOrder l_stop_hi = mk_exit(/*stop=*/102, /*limit=*/kNaN, kNaN, kNaN);
    CHECK(exit_order_earliest_path_metric_no_trail(
              wide, l_stop_hi, PositionSide::LONG, /*is_entry_bar=*/true,
              /*entry=*/100) == inf);

    // LONG entry@100, limit BELOW entry -> wrong side -> blocked.
    PendingOrder l_lim_lo = mk_exit(/*stop=*/kNaN, /*limit=*/98, kNaN, kNaN);
    CHECK(exit_order_earliest_path_metric_no_trail(
              wide, l_lim_lo, PositionSide::LONG, true, 100) == inf);

    // SHORT entry@100, stop BELOW entry -> wrong side -> blocked.
    PendingOrder s_stop_lo = mk_exit(/*stop=*/98, /*limit=*/kNaN, kNaN, kNaN);
    CHECK(exit_order_earliest_path_metric_no_trail(
              wide, s_stop_lo, PositionSide::SHORT, true, 100) == inf);

    // SHORT entry@100, limit ABOVE entry -> wrong side -> blocked.
    PendingOrder s_lim_hi = mk_exit(/*stop=*/kNaN, /*limit=*/102, kNaN, kNaN);
    CHECK(exit_order_earliest_path_metric_no_trail(
              wide, s_lim_hi, PositionSide::SHORT, true, 100) == inf);

    // Bar (100,105,95,100) has |H-O| == |O-L| == 5 -> TIE -> low-first path
    //   O(100) -> L(95) -> H(105) -> C(100).

    // LONG entry@100, stop BELOW entry (correct side) -> NOT blocked, finite.
    //   A long stop fires on a falling leg: O->L (100->95). stop=98 lands at
    //   pos 0 + (98-100)/(95-100) = 0.4, minus a 1e-15 nudge.
    PendingOrder l_ok = mk_exit(/*stop=*/98, /*limit=*/kNaN, kNaN, kNaN);
    double m_ok = exit_order_earliest_path_metric_no_trail(
        wide, l_ok, PositionSide::LONG, /*is_entry_bar=*/true, 100);
    CHECK(std::isfinite(m_ok));
    CHECK(near(m_ok, 0.4, 1e-6));

    // SHORT stop above entry on a NON-entry bar walks the path (no open gap:
    //   short gaps only when open >= stop, and 100 < 102). A short stop fires
    //   on a rising leg: L->H (95->105). stop=102 lands at
    //   pos 1 + (102-95)/(105-95) = 1.7, minus a 1e-15 nudge.
    PendingOrder s_walk = mk_exit(/*stop=*/102, /*limit=*/kNaN, kNaN, kNaN);
    double m_walk = exit_order_earliest_path_metric_no_trail(
        wide, s_walk, PositionSide::SHORT, /*is_entry_bar=*/false, 100);
    CHECK(near(m_walk, 1.7, 1e-6));

    // NON-entry-bar open gap: a LONG stop the bar opens straight through fills
    //   at the open -> metric 0. Bar open=96 <= stop=98.
    Bar open_gap = mk(96, 99, 95, 97);
    PendingOrder l_gap = mk_exit(/*stop=*/98, /*limit=*/kNaN, kNaN, kNaN);
    double m_open_gap = exit_order_earliest_path_metric_no_trail(
        open_gap, l_gap, PositionSide::LONG, /*is_entry_bar=*/false, 100);
    CHECK(near(m_open_gap, 0.0));

    // A trail order opts out of this metric entirely -> +inf.
    PendingOrder trail = mk_exit(/*stop=*/98, /*limit=*/kNaN,
                                 /*trail_points=*/10, /*trail_offset=*/kNaN);
    CHECK(exit_order_earliest_path_metric_no_trail(
              wide, trail, PositionSide::LONG, false, 100) == inf);
}

int main() {
    test_price_path_priority_branches();
    test_exit_order_touch_gap_arms();
    test_path_cross_kind_priority_order();
    test_resolve_exit_trail_fills();
    test_entry_bar_blocks_no_trail_exit();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
