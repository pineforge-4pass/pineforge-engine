#include "l4c_native_route_guard.hpp"
#define PineStrategyHost PineNativeHost
#define signed_position_size live_position_size
#include "oracle_fixture_config_shim.hpp"
/*
 * test_trail_fill_snap.cpp — trailing-exit fill rules pinned from TradingView
 * tapes in round 7 (family G, stevenygabbyperez-fast-scalper-with-stops:
 * strategy.exit(stop=close*0.99|1.01, trail_points=close*0.02/syminfo.mintick,
 * NO trail_offset); `lab tv` tapes scratchpad/r7/pins/scalper-trail-* and
 * trail-eq-*, record scratchpad/r7/pins/scalper-PINS.md). TV is ground truth.
 *
 * (1) A one-shot (omitted / zero-offset) trail whose activation the bar's
 *     OPEN already sits past fills at open -/+ 0 as a LEVEL: snapped
 *     directionally (sell floor, buy ceil), not nearest-rounded like a raw
 *     print the order gapped through.
 *       NASDAQ:AAPL 15m long 2025-04-21 19:45Z @191.91 (trail_points 383.82
 *         -> 384t, activation 195.75); 04-22 13:30Z opens 196.135: TV 196.13
 *         with the offset omitted AND with trail_offset=0; 196.12 with
 *         trail_offset=1 (196.135 - 1t = 196.125, floored). Engine booked
 *         bar_fill_price(196.135) = 196.14.
 *       NASDAQ:AAPL 15m short 05-22 17:30Z @201.9 (403.8 -> 404t, 197.86);
 *         05-23 13:30Z opens 193.665: TV 193.67 (ceil == nearest here).
 *
 * (2) trail_points is a tick count with a TOLERANT ceil (kTrailPointsCeilEps
 *     = 5e-5), trail_offset an EXACT floor: NYSE:F 15m short from the 04-02
 *     19:00Z signal (entry 10.11, mintick 0.01), zero-offset trails filling
 *     at the activation: trail_points 14.00001 -> 14t (9.97), 14.0001 /
 *     14.001 -> 15t (9.96), 0.14/syminfo.mintick = 14.000000000000002 -> 14t,
 *     14.0000001 -> 14t, 18.2 -> 19t (9.92); trail_offset
 *     0.3/(syminfo.mintick*10) = 2.9999999999999996 -> 2t (04-03 14:15Z
 *     @9.85 = trough 9.83 + 2t; 3t would print 9.86).
 *       BINANCE:BTCUSDT 15m short 2025-08-17 23:30Z @117559.99,
 *         trail_points = 117560 * 0.02 / 0.01 = 235120.00000000003: TV exits
 *         08-18 03:30Z @115208.79 (235120t); std::ceil gave 235121t -> .78.
 *
 * (3) The activation level and the trailing level are ON the tick grid: a bar
 *     extreme landing exactly on the level touches it. 10.11 - 21 * 0.01 is
 *     9.899999999999999 in doubles, one ulp UNDER the 9.9 low of NYSE:F
 *     2025-04-03 13:45Z (O 10.165 H 10.18 L 9.9 C 9.9): TV fills the
 *     zero-offset trail there @9.90 (trail-eq-S-off0-tp21; the probe's own
 *     20.22 -> 21t case); the engine read the leg as "not reached" and
 *     gap-filled the next open @9.89. The "stop == trough == close equality"
 *     reading of that row is REFUTED: trail_points 18 fills the same bar at
 *     its activation 9.93, not at the 9.90 extreme/close (trail-eq-S-off0,
 *     and trail-eq-S-omit with the offset omitted); the long twin (entry
 *     04-01 19:15Z @9.88, trail_points 8) fills 04-02 13:30Z @9.96 = the
 *     activation on a close == high bar (trail-eq-L-off0). A whole-tick
 *     offset trails durably and its level is touched inclusively: with
 *     trail_offset=1 the short fills 04-03 14:00Z @9.90 (trough 9.89 at the
 *     open + 1t == the bar's 9.90 high; trail-eq-S-off1), the long fills
 *     04-02 13:45Z @9.97 (peak 9.985 - 1t = 9.975, floored; trail-eq-L-off1).
 *
 * One-bar pins run the product probe (tests/trail_exit_product_probe.hpp:
 * the adapter's lowering, the kernel consumer's walk, the fill read from the
 * kernel's public event record — R5 lane P9 retired the TradingView exit-path
 * resolver these rows used to call); engine-level pins run the host end to
 * end over the registry feed bars (`lab bars`).
 */

#include <pineforge/source/pine_strategy_host.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "trail_exit_product_probe.hpp"
#include <pineforge/compat/pine/trail_ticks.hpp>

using namespace pineforge;
using namespace pineforge::trail_probe;
using namespace pineforge::compat::pine;

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

// ── registry feed bars (UTC labels; lab bars) ─────────────────────────

// NASDAQ:AAPL 15m, feed ae2b03d3736f.
const Bar kAaplSignal0421_1930 = mk(191.13, 192.09, 191.06, 191.92, 1745263800000);
const Bar kAaplEntry0421_1945  = mk(191.91, 193.43, 191.61, 193.03, 1745264700000);
const Bar kAapl0422_1330       = mk(196.135, 197.5, 195.96, 197.25, 1745328600000);
const Bar kAapl0422_1345       = mk(197.28, 197.855, 197.14, 197.81, 1745329500000);
const Bar kAaplSignal0522_1715 = mk(201.3, 201.94, 201.28, 201.88, 1747934100000);
const Bar kAaplEntry0522_1730  = mk(201.9, 202.08, 201.67, 202.06, 1747935000000);
const Bar kAapl0522_1745 = mk(202.09, 202.18, 201.78, 201.89, 1747935900000);
const Bar kAapl0522_1800 = mk(201.89, 202.11, 201.69, 202.07, 1747936800000);
const Bar kAapl0522_1815 = mk(202.07, 202.22, 201.91, 201.97, 1747937700000);
const Bar kAapl0522_1830 = mk(201.96, 201.96, 201.64, 201.72, 1747938600000);
const Bar kAapl0522_1845 = mk(201.71, 202.25, 201.69, 202.13, 1747939500000);
const Bar kAapl0522_1900 = mk(202.13, 202.68, 202.12, 202.52, 1747940400000);
const Bar kAapl0522_1915 = mk(202.51, 202.75, 202.11, 202.61, 1747941300000);
const Bar kAapl0522_1930 = mk(202.62, 202.65, 201.78, 201.82, 1747942200000);
const Bar kAapl0522_1945 = mk(201.82, 202.17, 201.0, 201.34, 1747943100000);
const Bar kAapl0523_1330 = mk(193.665, 197.095, 193.47, 196.0, 1748007000000);
const Bar kAapl0523_1345 = mk(195.97, 196.8, 195.32, 196.38, 1748007900000);

// NYSE:F 15m, feed 80f404ae85ef.
const Bar kFSignal0402_1900 = mk(10.13, 10.145, 10.105, 10.105, 1743620400000);
const Bar kFEntry0402_1915  = mk(10.105, 10.125, 10.085, 10.095, 1743621300000);
const Bar kF0402_1930 = mk(10.09, 10.125, 10.075, 10.115, 1743622200000);
const Bar kF0402_1945 = mk(10.115, 10.145, 10.1, 10.14, 1743623100000);
const Bar kF0403_1330 = mk(10.01, 10.2, 9.95, 10.17, 1743687000000);
const Bar kF0403_1345 = mk(10.165, 10.18, 9.9, 9.9, 1743687900000);
const Bar kF0403_1400 = mk(9.89, 9.9, 9.83, 9.835, 1743688800000);
const Bar kF0403_1415 = mk(9.835, 9.865, 9.8, 9.805, 1743689700000);

const Bar kFSignal0401_1900 = mk(9.84, 9.88, 9.84, 9.88, 1743534000000);
const Bar kFEntry0401_1915  = mk(9.88, 9.905, 9.87, 9.885, 1743534900000);
const Bar kF0401_1930 = mk(9.88, 9.9, 9.87, 9.9, 1743535800000);
const Bar kF0401_1945 = mk(9.9, 9.93, 9.87, 9.92, 1743536700000);
const Bar kF0402_1330 = mk(9.835, 9.985, 9.83, 9.985, 1743600600000);
const Bar kF0402_1345 = mk(9.98, 10.02, 9.945, 9.985, 1743601500000);
const Bar kF0402_1400 = mk(9.98, 10.08, 9.97, 10.06, 1743602400000);

// BINANCE:BTCUSDT 15m, feed 6b54c44ac6de.
const Bar kBtcSignal0817_2315 = mk(117779.55, 117837.2, 117481.99, 117560, 1755472500000);
const Bar kBtcEntry0817_2330  = mk(117559.99, 117603.13, 117482.31, 117569.57, 1755473400000);
const Bar kBtc0817_2345 = mk(117569.56, 117569.57, 117371.65, 117405.01, 1755474300000);
const Bar kBtc0818_0000 = mk(117405.01, 117543.75, 117336.04, 117512.99, 1755475200000);
const Bar kBtc0818_0015 = mk(117512.98, 117512.98, 117088.29, 117142.44, 1755476100000);
const Bar kBtc0818_0030 = mk(117142.44, 117364, 117020, 117258.5, 1755477000000);
const Bar kBtc0818_0045 = mk(117258.5, 117347.46, 117167.63, 117290.19, 1755477900000);
const Bar kBtc0818_0100 = mk(117290.2, 117400, 117264.91, 117304.29, 1755478800000);
const Bar kBtc0818_0115 = mk(117304.29, 117409.87, 116670.03, 116670.05, 1755479700000);
const Bar kBtc0818_0130 = mk(116670.04, 116935.12, 116366, 116427.6, 1755480600000);
const Bar kBtc0818_0145 = mk(116427.59, 116478.82, 116166.03, 116269.54, 1755481500000);
const Bar kBtc0818_0200 = mk(116269.53, 116339.61, 115910.99, 115995.2, 1755482400000);
const Bar kBtc0818_0215 = mk(115995.2, 116030, 115730.03, 115791.95, 1755483300000);
const Bar kBtc0818_0230 = mk(115791.94, 116070.75, 115678.02, 115896.15, 1755484200000);
const Bar kBtc0818_0245 = mk(115896.15, 115924.36, 115292.67, 115453.24, 1755485100000);
const Bar kBtc0818_0300 = mk(115452, 115636.89, 115433.69, 115480.01, 1755486000000);
const Bar kBtc0818_0315 = mk(115480, 115550.53, 115332, 115359.34, 1755486900000);
const Bar kBtc0818_0330 = mk(115359.33, 115417.13, 115115, 115166, 1755487800000);
const Bar kBtc0818_0345 = mk(115166, 115380, 115000, 115319, 1755488700000);

// The family's trail_points expression, evaluated the way the codegen'd
// strategy evaluates it: close * 0.02 / syminfo.mintick in doubles.
double scalper_trail_points(double signal_close, double mintick) {
    return signal_close * 0.02 / mintick;
}

// ── (2) tick arithmetic ───────────────────────────────────────────────

void test_trail_points_ceil_is_tolerant() {
    std::printf("test_trail_points_ceil_is_tolerant\n");
    // NYSE:F tapes: 14.00001 -> 14 (9.97), 14.0001 / 14.001 -> 15 (9.96).
    CHECK(trail_points_to_ticks(14.00001) == 14.0);
    CHECK(trail_points_to_ticks(14.0001) == 15.0);
    CHECK(trail_points_to_ticks(14.001) == 15.0);
    CHECK(trail_points_to_ticks(14.0000001) == 14.0);
    // 0.14 / syminfo.mintick evaluates to 14.000000000000002.
    CHECK(0.14 / 0.01 > 14.0);
    CHECK(trail_points_to_ticks(0.14 / 0.01) == 14.0);
    CHECK(trail_points_to_ticks(18.2) == 19.0);
    // The probes' own values.
    CHECK(trail_points_to_ticks(scalper_trail_points(10.11, 0.01)) == 21.0);
    CHECK(trail_points_to_ticks(scalper_trail_points(10.105, 0.01)) == 21.0);
    CHECK(trail_points_to_ticks(scalper_trail_points(191.92, 0.01)) == 384.0);
    CHECK(trail_points_to_ticks(scalper_trail_points(201.88, 0.01)) == 404.0);
    // BTC: 117560 * 0.02 / 0.01 = 235120.00000000003 -> 235120, not 235121.
    CHECK(scalper_trail_points(117560.0, 0.01) > 235120.0);
    CHECK(std::ceil(scalper_trail_points(117560.0, 0.01)) == 235121.0);
    CHECK(trail_points_to_ticks(scalper_trail_points(117560.0, 0.01)) == 235120.0);
    // Round-5 pins hold: a sub-tick trail_points still ceils to 1 tick.
    CHECK(trail_points_to_ticks(0.0006) == 1.0);
    CHECK(trail_points_to_ticks(0.6) == 1.0);
    CHECK(trail_points_to_ticks(3.0) == 3.0);
    CHECK(trail_points_to_ticks(0.0) == 0.0);
    CHECK(std::isnan(trail_points_to_ticks(kNaN)));
}

void test_trail_offset_floor_is_exact() {
    std::printf("test_trail_offset_floor_is_exact\n");
    // 0.3 / (syminfo.mintick * 10) evaluates to 2.9999999999999996 -> 2
    // ticks (trail-eq-S-off3fp2: 04-03 14:15Z @9.85 = 9.83 + 2t, not 9.86):
    // no tolerance on the floor, unlike the ceil.
    CHECK(0.3 / (0.01 * 10.0) < 3.0);
    CHECK(trail_offset_to_ticks(0.3 / (0.01 * 10.0)) == 2.0);
    CHECK(trail_offset_to_ticks(2.99999) == 2.0);
    CHECK(trail_offset_to_ticks(3.0) == 3.0);
    // Round-5 pins hold: [0, 1) is the zero-tick one-shot, 1.4 trails 1t.
    CHECK(trail_offset_to_ticks(0.0) == 0.0);
    CHECK(trail_offset_to_ticks(0.5) == 0.0);
    CHECK(trail_offset_to_ticks(0.9) == 0.0);
    CHECK(trail_offset_to_ticks(1.0) == 1.0);
    CHECK(trail_offset_to_ticks(1.4) == 1.0);
    CHECK(trail_offset_to_ticks(15.0) == 15.0);
    CHECK(std::isnan(trail_offset_to_ticks(kNaN)));
}

void test_trail_level_tick_grid_snap() {
    std::printf("test_trail_level_tick_grid_snap\n");
    // The F knife-edge: 10.11 - 21 * 0.01 sits one ulp under 9.9.
    const double raw = 10.11 - 21.0 * 0.01;
    CHECK(raw < 9.9);
    CHECK(snap_trail_level_to_tick_grid(raw, 0.01) == 9.9);
    // A genuinely sub-tick level stays raw (it takes the directional fill
    // snap downstream, 196.125 -> 196.12 for a sell).
    CHECK(snap_trail_level_to_tick_grid(196.135 - 0.01, 0.01) == 196.135 - 0.01);
    CHECK(near(196.135 - 0.01, 196.125));
    // XAUUSD round-5 shape on mintick 0.001: open - 15t materializes as the
    // grid point 3110.385.
    CHECK(near(snap_trail_level_to_tick_grid(3110.40 - 0.015, 0.001), 3110.385));
    CHECK(snap_trail_level_to_tick_grid(3110.40 - 0.015, 0.001) == 3110385.0 / 1000.0);
    CHECK(std::isnan(snap_trail_level_to_tick_grid(kNaN, 0.01)));
    CHECK(snap_trail_level_to_tick_grid(9.9, 0.0) == 9.9);
}

// ── (1) the product: the one-shot trail arming at a sub-tick open ─────

void test_product_aapl_long_arms_at_subtick_open() {
    std::printf("test_product_aapl_long_arms_at_subtick_open\n");
    // activation 191.91 + 384t = 195.75 < open 196.135. OMITTED offset: the
    // adapter lowers the one-shot to a kernel limit that is already inside
    // its region at the open, so the kernel fills it at the open PRINT
    // (PointPrice, raw 196.135) and the adapter's terms policy books the
    // trail's LEVEL — the open snapped directionally, 196.13 (TV), not the
    // nearest-rounded 196.14.
    const double tp = scalper_trail_points(191.92, 0.01);
    {
        TrailExitProjection f = trail_fill(kAapl0422_1330, PositionSide::LONG, tp, kNaN,
                                           /*entry=*/191.91, /*best_start=*/193.43,
                                           /*mintick=*/0.01);
        CHECK(f.filled == true);
        CHECK(f.raw_price == 196.135);
        CHECK(f.at_bar_open == true);
        CHECK(f.level_fill == false);
        CHECK(f.leg_is_limit == true);
        CHECK(near(f.exit_price, 196.13));
        CHECK(near(f.path_position, 0.0));
    }
    // EXPLICIT 0 (round 10 family AC, test_zero_offset_trail_rides): the same
    // one-shot lowering until the activation is reached, the same open print,
    // the same booked 196.13 (the aapl-pre-tp100 tape refutes the print
    // rounding's 196.14). expectation corrected: a path LEVEL fill at
    // position 0 that is not at the open (the resolver's is_trail = true,
    // at_bar_open = false) -> the one-shot limit's open print, because the
    // resolver spelled the explicit-zero trail as arming at the open and
    // crossing its own level at once, while the product has one lowering for
    // the omitted and the zero offset; TradingView's 196.13 is the same.
    {
        TrailExitProjection f = trail_fill(kAapl0422_1330, PositionSide::LONG, tp, 0.0,
                                           /*entry=*/191.91, /*best_start=*/193.43,
                                           /*mintick=*/0.01);
        CHECK(f.filled == true);
        CHECK(f.raw_price == 196.135);
        CHECK(f.at_bar_open == true);
        CHECK(f.level_fill == false);
        CHECK(f.leg_is_limit == true);
        CHECK(near(f.exit_price, 196.13));
        CHECK(near(f.path_position, 0.0));
    }
    // trail_offset=1: a generic kernel Trail, armed at the open with best =
    // open; the adverse-first leg (|O-L| = 0.175 < |H-O| = 1.365) crosses
    // 196.135 - 1t = 196.125 — a level crossing (TV books the floored 196.12).
    TrailExitProjection f1 = trail_fill(kAapl0422_1330, PositionSide::LONG, tp, 1.0,
                                        191.91, 193.43, 0.01);
    CHECK(f1.filled == true);
    CHECK(near(f1.raw_price, 196.125));
    CHECK(f1.leg_is_trail == true);
    CHECK(f1.level_fill == true);
    CHECK(f1.at_bar_open == false);
    CHECK(near(f1.exit_price, 196.12));
}

void test_product_aapl_short_arms_at_subtick_open() {
    std::printf("test_product_aapl_short_arms_at_subtick_open\n");
    // activation 201.9 - 404t = 197.86 > open 193.665: the one-shot's open
    // print, booked as the level's directional snap 193.67 (== nearest here).
    const double tp = scalper_trail_points(201.88, 0.01);
    TrailExitProjection f = trail_fill(kAapl0523_1330, PositionSide::SHORT, tp, kNaN,
                                       /*entry=*/201.9, /*best_start=*/201.0,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(f.raw_price == 193.665);
    CHECK(f.at_bar_open == true);
    CHECK(f.level_fill == false);
    CHECK(near(f.exit_price, 193.67));
}

void test_product_adverse_gap_through_armed_level_is_a_raw_print() {
    std::printf("test_product_adverse_gap_through_armed_level_is_a_raw_print\n");
    // Control: a trail ARMED from the carried best (omitted offset, best 99.5
    // past the 99.97 activation) is marketable at the next open in the
    // adapter's lowering (native_order::Market): the open the order gapped
    // through in the adverse direction is a raw PRINT, booked as such (the
    // #148 / corpus discriminator booking, unchanged).
    Bar gap_up = mk(100.20, 100.30, 100.05, 100.10);
    TrailExitProjection f = trail_fill(gap_up, PositionSide::SHORT,
                                       /*trail_points=*/3.0, /*trail_offset=*/kNaN,
                                       /*entry=*/100.0, /*best_start=*/99.5,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(near(f.raw_price, 100.20));
    CHECK(f.at_bar_open == true);
    CHECK(f.leg_is_market == true);
    CHECK(f.level_fill == false);
    CHECK(near(f.exit_price, 100.20));
}

// ── (3) the product: the activation is a tick-grid level ──────────────

void test_product_ford_short_activation_touched_by_the_low() {
    std::printf("test_product_ford_short_activation_touched_by_the_low\n");
    // 21t from 10.11 -> 9.90 == the bar's low (high-first path O->H->L->C).
    // The adapter's one-shot limit rests at the half-tick threshold 9.905 —
    // where the tick-quantized path first prints 9.90 — which the H->L leg
    // crosses just before its end; the adapter books the activation 9.90
    // (TV fills @9.90 on this bar). The probe's own trail_points (20.22) and
    // a literal 21 agree.
    const double tps[] = {scalper_trail_points(10.11, 0.01), 21.0};
    for (double tp : tps) {
        TrailExitProjection f = trail_fill(kF0403_1345, PositionSide::SHORT, tp, kNaN,
                                           /*entry=*/10.11, /*best_start=*/9.95,
                                           /*mintick=*/0.01);
        CHECK(f.filled == true);
        CHECK(f.exit_price == 9.9);
        CHECK(f.leg_is_limit == true);
        CHECK(f.level_fill == true);
        CHECK(f.at_bar_open == false);
        CHECK(near(f.raw_price, 9.905));
        // expectation corrected: path position 2.0 (the resolver's H->L leg
        // ENDING on the level) -> 1 + (10.18 - 9.905) / (10.18 - 9.9), because
        // the kernel's cursor is the raw path's crossing of the adapter's
        // threshold, half a tick before the low; the booked price and bar are
        // the same.
        CHECK(near(f.path_position, 1.0 + (10.18 - 9.905) / (10.18 - 9.9), 1e-9));
    }
    // Explicit 0 is the same one-shot.
    TrailExitProjection f0 = trail_fill(kF0403_1345, PositionSide::SHORT, 21.0, 0.0,
                                        10.11, 9.95, 0.01);
    CHECK(f0.filled == true);
    CHECK(f0.exit_price == 9.9);
}

void test_product_ford_short_fills_at_activation_not_at_the_extreme() {
    std::printf("test_product_ford_short_fills_at_activation_not_at_the_extreme\n");
    // trail_points 18 -> activation 9.93, crossed on the H->L leg: the fill
    // is the activation (TV 9.93, trail-eq-S-off0 / trail-eq-S-omit), NOT
    // the 9.90 trough / close. Refutes the "stop == trough == close"
    // equality reading of the probe row.
    const double offsets[] = {kNaN, 0.0};
    for (double off : offsets) {
        TrailExitProjection f = trail_fill(kF0403_1345, PositionSide::SHORT, 18.0, off,
                                           10.11, 9.95, 0.01);
        CHECK(f.filled == true);
        CHECK(near(f.exit_price, 9.93));
        CHECK(f.leg_is_limit == true);
        CHECK(f.level_fill == true);
        CHECK(near(f.raw_price, 9.935));
        // expectation corrected: 1 + (9.93 - 10.18) / (9.9 - 10.18) ->
        // 1 + (9.935 - 10.18) / (9.9 - 10.18), because the crossing the
        // kernel records is the half-tick threshold's, not the level's.
        CHECK(near(f.path_position, 1.0 + (9.935 - 10.18) / (9.9 - 10.18), 1e-9));
    }
    // Tolerant ceil through the adapter: 14.00001 -> 14t = 9.97 on the
    // 13:30Z bar (low-first path, the O->L leg 10.01 -> 9.95 crosses it);
    // 14.0001 -> 15t = 9.96. 0.14 / syminfo.mintick -> 9.97.
    TrailExitProjection a = trail_fill(kF0403_1330, PositionSide::SHORT, 14.00001, 0.0,
                                       10.11, 10.075, 0.01);
    CHECK(a.filled == true);
    CHECK(near(a.exit_price, 9.97));
    TrailExitProjection b = trail_fill(kF0403_1330, PositionSide::SHORT, 14.0001, 0.0,
                                       10.11, 10.075, 0.01);
    CHECK(b.filled == true);
    CHECK(near(b.exit_price, 9.96));
    TrailExitProjection c = trail_fill(kF0403_1330, PositionSide::SHORT, 0.14 / 0.01, 0.0,
                                       10.11, 10.075, 0.01);
    CHECK(c.filled == true);
    CHECK(near(c.exit_price, 9.97));
}

void test_product_ford_short_whole_tick_offset_level_touched_by_the_high() {
    std::printf("test_product_ford_short_whole_tick_offset_level_touched_by_the_high\n");
    // trail_offset=1: the 13:45Z bar arms at 9.93 and trails to 9.90 + 1t =
    // 9.91 (close 9.90: no fill). 14:00Z opens 9.89 (new trough, level
    // 9.90), the O->H leg ends ON 9.90 -> fill @9.90 (TV trail-eq-S-off1): a
    // generic kernel Trail whose stop the leg's end touches.
    TrailExitProjection hold = trail_fill(kF0403_1345, PositionSide::SHORT, 18.0, 1.0,
                                          10.11, 9.95, 0.01);
    CHECK(hold.filled == false);
    TrailExitProjection f = trail_fill(kF0403_1400, PositionSide::SHORT, 18.0, 1.0,
                                       10.11, /*best_start=*/9.9, 0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 9.9));
    CHECK(f.leg_is_trail == true);
    CHECK(f.level_fill == true);
    CHECK(near(f.path_position, 1.0));
    // trail_offset = 0.3 / (syminfo.mintick * 10) (2.9999999999999996 ->
    // 2t, exact floor): 14:00Z trails 9.89 + 2t = 9.91 (high 9.90: hold),
    // trough 9.83 -> 9.85 (close 9.835: hold); 14:15Z opens 9.835, the
    // O->H leg (high-first: |H-O| = 0.03 < |O-L| = 0.035) reaches 9.85 ->
    // fill @9.85 (TV trail-eq-S-off3fp2; a tolerant 3t floor would print
    // 9.86). The kernel Trail, already reached at placement, restarts its
    // best at the 9.835 open (raw stop 9.855); the adapter's terms policy
    // books TradingView's carried level 9.83 + 2t.
    const double off3 = 0.3 / (0.01 * 10.0);
    TrailExitProjection h1 = trail_fill(kF0403_1345, PositionSide::SHORT, 18.0, off3,
                                        10.11, 9.95, 0.01);
    CHECK(h1.filled == false);
    TrailExitProjection h2 = trail_fill(kF0403_1400, PositionSide::SHORT, 18.0, off3,
                                        10.11, 9.9, 0.01);
    CHECK(h2.filled == false);
    TrailExitProjection g = trail_fill(kF0403_1415, PositionSide::SHORT, 18.0, off3,
                                       10.11, 9.83, 0.01);
    CHECK(g.filled == true);
    CHECK(near(g.exit_price, 9.85));
    CHECK(g.leg_is_trail == true);
    CHECK(near(g.raw_price, 9.855));
}

void test_product_ford_long_twin() {
    std::printf("test_product_ford_long_twin\n");
    // Long @9.88, trail_points 8 -> 9.96. 04-02 13:30Z (O 9.835 H 9.985
    // L 9.83 C 9.985, low-first) crosses it on the L->H leg: fill @9.96
    // (TV trail-eq-L-off0), not the 9.985 peak == close. The one-shot limit
    // rests at the half-tick threshold 9.955.
    TrailExitProjection f = trail_fill(kF0402_1330, PositionSide::LONG, 8.0, 0.0,
                                       /*entry=*/9.88, /*best_start=*/9.93, 0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 9.96));
    CHECK(f.leg_is_limit == true);
    CHECK(f.level_fill == true);
    CHECK(near(f.raw_price, 9.955));
    // expectation corrected: 1 + (9.96 - 9.83) / (9.985 - 9.83) ->
    // 1 + (9.955 - 9.83) / (9.985 - 9.83), because the kernel's cursor is
    // the threshold crossing, half a tick before the level.
    CHECK(near(f.path_position, 1.0 + (9.955 - 9.83) / (9.985 - 9.83), 1e-9));
    // trail_offset=1: arms at 9.96, trails the 9.985 peak - 1t = 9.975
    // (close 9.985: hold); 13:45Z (O 9.98, low-first) crosses the level on
    // the O->L leg — TV trail-eq-L-off1 @9.97. The kernel Trail, already
    // reached at placement, restarts its best at the 9.98 open, so its own
    // stop is 9.97 exactly; the booked 9.97 is TradingView's floored 9.975.
    // expectation corrected: raw level 9.975 -> 9.97, because the resolver
    // read the carried best 9.985 into its level while the kernel's best
    // begins at the first live print; the booked price and bar are the same.
    TrailExitProjection hold = trail_fill(kF0402_1330, PositionSide::LONG, 8.0, 1.0,
                                          9.88, 9.93, 0.01);
    CHECK(hold.filled == false);
    TrailExitProjection g = trail_fill(kF0402_1345, PositionSide::LONG, 8.0, 1.0,
                                       9.88, /*best_start=*/9.985, 0.01);
    CHECK(g.filled == true);
    CHECK(near(g.exit_price, 9.97));
    CHECK(g.leg_is_trail == true);
    CHECK(near(g.raw_price, 9.97));
}

void test_product_btc_short_activation_after_tolerant_ceil() {
    std::printf("test_product_btc_short_activation_after_tolerant_ceil\n");
    // 235120t from 117559.99 -> 115208.79, crossed on the O->H->L->C path's
    // H->L leg of 08-18 03:30Z. std::ceil's 235121t would put it at .78.
    const double tp = scalper_trail_points(117560.0, 0.01);
    TrailExitProjection f = trail_fill(kBtc0818_0330, PositionSide::SHORT, tp, kNaN,
                                       /*entry=*/117559.99, /*best_start=*/115292.67,
                                       /*mintick=*/0.01);
    CHECK(f.filled == true);
    CHECK(near(f.exit_price, 115208.79));
    CHECK(f.leg_is_limit == true);
    CHECK(f.level_fill == true);
    TrailExitProjection g = trail_fill(kBtc0818_0330, PositionSide::SHORT, 235121.0, kNaN,
                                       117559.99, 115292.67, 0.01);
    CHECK(g.filled == true);
    CHECK(near(g.exit_price, 115208.78));
}

// ── engine-level fixtures ─────────────────────────────────────────────

class TrailEngine : public pineforge::source::PineStrategyHost {
public:
    double exit_price(int i) const { return closed_trade_exit_price(i); }
    double entry_price(int i) const { return closed_trade_entry_price(i); }
    int exit_bar(int i) const { return closed_trade_exit_bar_index(i); }
    double position_size() const { return signed_position_size(); }
};

// The family's shape: a market entry on the signal bar (fills at the next
// open) with strategy.exit(stop=close*0.99|1.01, trail_points=..., [offset])
// issued alongside it; the exit rests until it fills.
class ScalperTrailProbe : public TrailEngine {
public:
    ScalperTrailProbe(bool is_long, double trail_points, double trail_offset,
                      double mintick, bool with_stop = true)
        : is_long_(is_long), trail_points_(trail_points),
          trail_offset_(trail_offset), with_stop_(with_stop) {
        initial_capital_ = 1'000'000.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1.0;
        commission_value_ = 0.0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        syminfo_mintick_ = mintick;
    }

    void on_source_bar(const Bar& bar) override {
        if (bar_index_ == 0) {
            strategy_entry("E", is_long_, kNaN, kNaN, /*qty=*/1.0);
            const double stop = with_stop_
                ? (is_long_ ? bar.close * 0.99 : bar.close * 1.01)
                : kNaN;
            strategy_exit("x", "E", /*limit=*/kNaN, stop,
                          trail_points_, trail_offset_, /*trail_price=*/kNaN);
        }
    }

private:
    bool is_long_;
    double trail_points_;
    double trail_offset_;
    bool with_stop_;
};

struct Outcome {
    int trades;
    double entry_price;
    double exit_price;
    int exit_bar;
    double position;
    std::string error;
};

Outcome run_probe(bool is_long, double trail_points, double trail_offset,
                  double mintick, const std::vector<Bar>& bars,
                  bool with_stop = true) {
    ScalperTrailProbe eng(is_long, trail_points, trail_offset, mintick, with_stop);
    eng.run(bars.data(), (int)bars.size());
    Outcome o{eng.trade_count(), kNaN, kNaN, -1, eng.position_size(),
              eng.last_error()};
    if (eng.trade_count() >= 1) {
        o.entry_price = eng.entry_price(0);
        o.exit_price = eng.exit_price(0);
        o.exit_bar = eng.exit_bar(0);
    }
    return o;
}

void test_engine_aapl_long_exit_floors_at_subtick_open() {
    std::printf("test_engine_aapl_long_exit_floors_at_subtick_open\n");
    const std::vector<Bar> bars = {
        kAaplSignal0421_1930, kAaplEntry0421_1945, kAapl0422_1330, kAapl0422_1345,
    };
    const double tp = scalper_trail_points(191.92, 0.01);
    // Omitted offset and explicit 0: TV 196.13 (was 196.14).
    const double one_shot[] = {kNaN, 0.0};
    for (double off : one_shot) {
        Outcome o = run_probe(true, tp, off, 0.01, bars);
        CHECK(o.error.empty());
        CHECK(o.trades == 1);
        CHECK(near(o.entry_price, 191.91));
        CHECK(near(o.exit_price, 196.13));
        CHECK(o.exit_bar == 2);
        CHECK(near(o.position, 0.0));
    }
    // trail_offset=1: 196.135 - 1t = 196.125 floored -> 196.12 (TV).
    Outcome o1 = run_probe(true, tp, 1.0, 0.01, bars);
    CHECK(o1.error.empty());
    CHECK(o1.trades == 1);
    CHECK(near(o1.exit_price, 196.12));
    CHECK(o1.exit_bar == 2);
}

void test_engine_aapl_short_exit_ceils_at_subtick_open() {
    std::printf("test_engine_aapl_short_exit_ceils_at_subtick_open\n");
    const std::vector<Bar> bars = {
        kAaplSignal0522_1715, kAaplEntry0522_1730, kAapl0522_1745, kAapl0522_1800,
        kAapl0522_1815, kAapl0522_1830, kAapl0522_1845, kAapl0522_1900,
        kAapl0522_1915, kAapl0522_1930, kAapl0522_1945, kAapl0523_1330,
        kAapl0523_1345,
    };
    const double tp = scalper_trail_points(201.88, 0.01);
    Outcome o = run_probe(false, tp, kNaN, 0.01, bars);
    CHECK(o.error.empty());
    CHECK(o.trades == 1);
    CHECK(near(o.entry_price, 201.9));
    CHECK(near(o.exit_price, 193.67));   // TV (ceil == nearest for 193.665)
    CHECK(o.exit_bar == 11);
    CHECK(near(o.position, 0.0));
}

void test_engine_ford_short_activation_touch_and_tolerant_ticks() {
    std::printf("test_engine_ford_short_activation_touch_and_tolerant_ticks\n");
    const std::vector<Bar> bars = {
        kFSignal0402_1900, kFEntry0402_1915, kF0402_1930, kF0402_1945,
        kF0403_1330, kF0403_1345, kF0403_1400, kF0403_1415,
    };
    // The probe's row: entry 10.105 -> 10.11 (nearest), 20.21 -> 21t ->
    // activation 9.90 touched by the 13:45Z low -> exit @9.90 on bar 5
    // (was the 14:00Z open 9.89 on bar 6).
    Outcome probe = run_probe(false, scalper_trail_points(10.105, 0.01), kNaN,
                              0.01, bars);
    CHECK(probe.error.empty());
    CHECK(probe.trades == 1);
    CHECK(near(probe.entry_price, 10.11));
    CHECK(near(probe.exit_price, 9.90));
    CHECK(probe.exit_bar == 5);
    CHECK(near(probe.position, 0.0));
    // The synthetic pins (trail-eq-*), same bars, offsets NaN / 0 / 1 / fp3.
    struct Pin { double tp; double off; double price; int bar; };
    const Pin pins[] = {
        {18.0, kNaN, 9.93, 5},          // trail-eq-S-omit
        {18.0, 0.0, 9.93, 5},           // trail-eq-S-off0
        {21.0, 0.0, 9.90, 5},           // trail-eq-S-off0-tp21
        {18.2, 0.0, 9.92, 5},           // trail-eq-S-off0-tp18p2
        {18.0, 1.0, 9.90, 6},           // trail-eq-S-off1 (14:00Z high 9.90)
        {18.0, 0.3 / (0.01 * 10.0), 9.85, 7},  // trail-eq-S-off3fp2 (14:15Z)
        {14.00001, 0.0, 9.97, 4},       // trail-eq-S-off0-tp1400001 (13:30Z)
        {14.0001, 0.0, 9.96, 4},        // trail-eq-S-off0-tp140001
        {14.001, 0.0, 9.96, 4},         // trail-eq-S-off0-tp14001
        {0.14 / 0.01, 0.0, 9.97, 4},    // trail-eq-S-off0-fpceil
        {14.0000001, 0.0, 9.97, 4},     // trail-eq-S-off0-fpceil2
    };
    for (const Pin& p : pins) {
        Outcome o = run_probe(false, p.tp, p.off, 0.01, bars, /*with_stop=*/false);
        CHECK(o.error.empty());
        CHECK(o.trades == 1);
        CHECK(near(o.entry_price, 10.11));
        CHECK(near(o.exit_price, p.price));
        CHECK(o.exit_bar == p.bar);
        CHECK(near(o.position, 0.0));
    }
}

void test_engine_ford_long_twin() {
    std::printf("test_engine_ford_long_twin\n");
    const std::vector<Bar> bars = {
        kFSignal0401_1900, kFEntry0401_1915, kF0401_1930, kF0401_1945,
        kF0402_1330, kF0402_1345, kF0402_1400,
    };
    Outcome o0 = run_probe(true, 8.0, 0.0, 0.01, bars, /*with_stop=*/false);
    CHECK(o0.error.empty());
    CHECK(o0.trades == 1);
    CHECK(near(o0.entry_price, 9.88));
    CHECK(near(o0.exit_price, 9.96));     // trail-eq-L-off0
    CHECK(o0.exit_bar == 4);
    Outcome o1 = run_probe(true, 8.0, 1.0, 0.01, bars, /*with_stop=*/false);
    CHECK(o1.error.empty());
    CHECK(o1.trades == 1);
    CHECK(near(o1.exit_price, 9.97));     // trail-eq-L-off1 (9.975 floored)
    CHECK(o1.exit_bar == 5);
}

void test_engine_btc_short_exit_at_the_tolerant_activation() {
    std::printf("test_engine_btc_short_exit_at_the_tolerant_activation\n");
    const std::vector<Bar> bars = {
        kBtcSignal0817_2315, kBtcEntry0817_2330, kBtc0817_2345, kBtc0818_0000,
        kBtc0818_0015, kBtc0818_0030, kBtc0818_0045, kBtc0818_0100,
        kBtc0818_0115, kBtc0818_0130, kBtc0818_0145, kBtc0818_0200,
        kBtc0818_0215, kBtc0818_0230, kBtc0818_0245, kBtc0818_0300,
        kBtc0818_0315, kBtc0818_0330, kBtc0818_0345,
    };
    Outcome o = run_probe(false, scalper_trail_points(117560.0, 0.01), kNaN,
                          0.01, bars);
    CHECK(o.error.empty());
    CHECK(o.trades == 1);
    CHECK(near(o.entry_price, 117559.99));
    CHECK(near(o.exit_price, 115208.79));   // TV; was 115208.78
    CHECK(o.exit_bar == 17);
    CHECK(near(o.position, 0.0));
}

void test_engine_on_tick_open_and_resting_stop_gap_unchanged() {
    std::printf("test_engine_on_tick_open_and_resting_stop_gap_unchanged\n");
    // Control 1: a one-shot trail arming at an ON-TICK open books that open
    // (floor == nearest there): long @100.00, activation 100.30, bar 2 opens
    // 100.50.
    const std::vector<Bar> on_tick = {
        mk(100.00, 100.10, 99.90, 100.00, 1000),
        mk(100.00, 100.20, 99.95, 100.10, 2000),
        mk(100.50, 100.80, 100.40, 100.70, 3000),
        mk(100.70, 100.90, 100.60, 100.80, 4000),
    };
    Outcome a = run_probe(true, 30.0, kNaN, 0.01, on_tick, /*with_stop=*/false);
    CHECK(a.error.empty());
    CHECK(a.trades == 1);
    CHECK(near(a.exit_price, 100.50));
    CHECK(a.exit_bar == 2);
    // Control 2: a resting STOP the open gaps through is still a raw print,
    // nearest-rounded (finding-446): long @191.91 with a sell stop 196.50
    // resting above the entry and the 04-22 13:30Z open 196.135 below it ->
    // 196.14, not the trail's 196.13.
    class StopGapProbe : public TrailEngine {
    public:
        StopGapProbe() {
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
                strategy_entry("E", true, kNaN, kNaN, /*qty=*/1.0);
            } else if (bar_index_ == 1) {
                strategy_exit("x", "E", /*limit=*/kNaN, /*stop=*/196.50);
            }
        }
    };
    StopGapProbe eng;
    const std::vector<Bar> bars = {
        kAaplSignal0421_1930, kAaplEntry0421_1945, kAapl0422_1330, kAapl0422_1345,
    };
    eng.run(bars.data(), (int)bars.size());
    CHECK(eng.last_error().empty());
    CHECK(eng.trade_count() == 1);
    if (eng.trade_count() == 1) {
        CHECK(near(eng.exit_price(0), 196.14));
        CHECK(eng.exit_bar(0) == 2);
    }
}

}  // namespace

int main() {
    std::printf("=== test_trail_fill_snap ===\n");

    test_trail_points_ceil_is_tolerant();
    test_trail_offset_floor_is_exact();
    test_trail_level_tick_grid_snap();

    test_product_aapl_long_arms_at_subtick_open();
    test_product_aapl_short_arms_at_subtick_open();
    test_product_adverse_gap_through_armed_level_is_a_raw_print();

    test_product_ford_short_activation_touched_by_the_low();
    test_product_ford_short_fills_at_activation_not_at_the_extreme();
    test_product_ford_short_whole_tick_offset_level_touched_by_the_high();
    test_product_ford_long_twin();
    test_product_btc_short_activation_after_tolerant_ceil();

    test_engine_aapl_long_exit_floors_at_subtick_open();
    test_engine_aapl_short_exit_ceils_at_subtick_open();
    test_engine_ford_short_activation_touch_and_tolerant_ticks();
    test_engine_ford_long_twin();
    test_engine_btc_short_exit_at_the_tolerant_activation();
    test_engine_on_tick_open_and_resting_stop_gap_unchanged();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
