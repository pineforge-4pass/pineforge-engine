/*
 * test_default_pct_stop_sizing.cpp — round 7, family K: TradingView's sizing
 * and admission of a DEFAULT percent_of_equity (<= 100) strategy.entry(stop=)
 * under margin simulation, pinned by four `lab tv` tapes on NYSE:F 15
 * 2025-08-11..23 (2026-09-05, ledger note log-20260905t084529z-c7b22df1;
 * tapes scratchpad/r7/pins/f15-stopsize-{pct100,pct50,short-only,short-m50},
 * decoder scratchpad/r7/k/aht_rule.py: 121/126 ahtisham F@15 TV entries
 * reproduced with qty and price, every non-fill) and by the ahtisham
 * volatility-expansion F@15 first-divergence rows (scratchpad/r7/k/).
 *
 * The pinned rule (default_qty_type = percent_of_equity, pct <= 100,
 * margin_long/short > 0, default process_orders_on_close):
 *
 *   1. SIZING at the call: qty = floor(equity * pct/100 / tick(level)) with
 *      the level snapped to the tick directionally (buy stop ceil, sell stop
 *      floor) — NOT at the close: pct100 fills 858 = floor(10,000 / 11.65)
 *      and 854 = floor(10,000 / 11.70) (873 / 869 at the closes); pct50
 *      shorts 450 / 444 / 441 = floor(0.5 eq / L); margin 50 shorts
 *      901 / 886 / 880 = floor(eq / L).
 *   2. PLACEMENT (family E) on that quantity: accepted iff
 *      qty * tick(close) * margin%/100 <= strategy.equity, so an all-in sell
 *      stop BELOW the close is never placed (floor(eq/L) * C > eq: 0 short
 *      fills over the 3 touches of pct100, 0 fills on short-only — no
 *      opposite-order/OCA effect) while a buy stop above the close always
 *      is; a rejected placement is dropped and only the script's next call
 *      re-issues it; a rejected same-id re-issue cancels the resting order.
 *   3. FILL: the same quantity at the level on a touch, at the tick-rounded
 *      open on a gap-through, admitted iff qty * tick(fill) <= equity
 *      (08-19 13:30Z: 817 = floor(9,414.16 / 11.51) x 11.52 = 9,411.84 <=
 *      9,414.16 fills where the close-sized 822 x 11.52 = 9,469 would not;
 *      a first-bar short gap-through is never filled because the order was
 *      never placed).
 *   4. A level already at/beyond the close is a market-at-next-open order
 *      sized at tick(close): ahtisham 2025-04-04 13:30Z close 9.335 -> 9.34,
 *      1,043 = floor(9,742.34 / 9.34) filled 13:45Z @9.34 (TV: 88 margin-
 *      called @9.44 + 955 stopped 15:00Z @9.52).
 *
 * Engine before this change (d9e15ab): KI-62 sized the stop at the FILL
 * price and costed it at the bar OPEN (engine_fills.cpp
 * stop_entry_margin_admission_declines), a next-open-only snapshot sized it
 * at the CLOSE. That coincided with TV on every intrabar touch and diverged
 * on every session-open gap: 18/18 first-bar SHORT gap-throughs filled that
 * TV never placed (04-04 13:30Z 1,020 @9.32), 0/19 first-bar LONG
 * gap-throughs filled of which TV fills 6.
 *
 * Feed bars are the registry's NYSE:F 15 (feed 80f404ae85ef, mintick 0.01,
 * whole shares), UTC, `lab bars`. Tape times are UTC+8 in the CSVs; quoted
 * here in UTC.
 */

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "oracle_fixture_config_shim.hpp"
#include "l4b_pending_projection_shim.hpp"

using namespace pineforge;
using pineforge::source::PendingOrder;

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

constexpr int64_t kMin15 = 15LL * 60LL * 1000LL;

struct Row { int64_t ts; double o, h, l, c; };

// NYSE:F 15, 2025-08-11 13:30Z .. 2025-08-22 19:45Z, 260 bars (10 sessions
// of 26). Index map (first bar of each session): 08-11 = 0, 08-12 = 26,
// 08-13 = 52, 08-14 = 78, 08-15 = 104, 08-18 = 130, 08-19 = 156,
// 08-20 = 182, 08-21 = 208, 08-22 = 234.
enum ABar {
    A0811_1415 = 3, A0811_1430 = 4, A0811_1445 = 5,
    A0813_1945 = 77, A0814_1330 = 78, A0814_1345 = 79,
    A0818_1945 = 155, A0819_1330 = 156, A0819_1345 = 157,
    A0820_1945 = 207, A0821_1330 = 208, A0821_1345 = 209,
    A0822_1400 = 236, A0822_1415 = 237,
};

const Row kF0811[] = {
    {1754919000000LL, 11.32, 11.57, 11.31, 11.535},   // 08-11 13:30Z
    {1754919900000LL, 11.535, 11.54, 11.35, 11.4},   // 08-11 13:45Z
    {1754920800000LL, 11.4, 11.405, 11.29, 11.295},   // 08-11 14:00Z
    {1754921700000LL, 11.3, 11.315, 11.275, 11.295},   // 08-11 14:15Z
    {1754922600000LL, 11.295, 11.38, 11.06, 11.16},   // 08-11 14:30Z
    {1754923500000LL, 11.15, 11.18, 11.14, 11.165},   // 08-11 14:45Z
    {1754924400000LL, 11.16, 11.21, 11.12, 11.13},   // 08-11 15:00Z
    {1754925300000LL, 11.13, 11.15, 11.11, 11.125},   // 08-11 15:15Z
    {1754926200000LL, 11.12, 11.125, 11.09, 11.12},   // 08-11 15:30Z
    {1754927100000LL, 11.12, 11.17, 11.11, 11.17},   // 08-11 15:45Z
    {1754928000000LL, 11.17, 11.21, 11.17, 11.195},   // 08-11 16:00Z
    {1754928900000LL, 11.195, 11.21, 11.18, 11.18},   // 08-11 16:15Z
    {1754929800000LL, 11.18, 11.19, 11.15, 11.155},   // 08-11 16:30Z
    {1754930700000LL, 11.155, 11.17, 11.13, 11.135},   // 08-11 16:45Z
    {1754931600000LL, 11.135, 11.15, 11.13, 11.13},   // 08-11 17:00Z
    {1754932500000LL, 11.13, 11.15, 11.09, 11.095},   // 08-11 17:15Z
    {1754933400000LL, 11.1, 11.14, 11.085, 11.12},   // 08-11 17:30Z
    {1754934300000LL, 11.12, 11.14, 11.115, 11.135},   // 08-11 17:45Z
    {1754935200000LL, 11.135, 11.14, 11.105, 11.115},   // 08-11 18:00Z
    {1754936100000LL, 11.115, 11.165, 11.115, 11.15},   // 08-11 18:15Z
    {1754937000000LL, 11.155, 11.165, 11.13, 11.13},   // 08-11 18:30Z
    {1754937900000LL, 11.13, 11.14, 11.13, 11.14},   // 08-11 18:45Z
    {1754938800000LL, 11.14, 11.14, 11.12, 11.135},   // 08-11 19:00Z
    {1754939700000LL, 11.14, 11.15, 11.125, 11.145},   // 08-11 19:15Z
    {1754940600000LL, 11.15, 11.16, 11.145, 11.155},   // 08-11 19:30Z
    {1754941500000LL, 11.155, 11.16, 11.125, 11.16},   // 08-11 19:45Z
    {1755005400000LL, 11.17, 11.2, 11.13, 11.135},   // 08-12 13:30Z
    {1755006300000LL, 11.135, 11.2, 11.135, 11.195},   // 08-12 13:45Z
    {1755007200000LL, 11.19, 11.28, 11.19, 11.275},   // 08-12 14:00Z
    {1755008100000LL, 11.27, 11.315, 11.265, 11.295},   // 08-12 14:15Z
    {1755009000000LL, 11.29, 11.29, 11.225, 11.265},   // 08-12 14:30Z
    {1755009900000LL, 11.265, 11.305, 11.26, 11.29},   // 08-12 14:45Z
    {1755010800000LL, 11.29, 11.31, 11.275, 11.285},   // 08-12 15:00Z
    {1755011700000LL, 11.29, 11.295, 11.26, 11.265},   // 08-12 15:15Z
    {1755012600000LL, 11.27, 11.28, 11.26, 11.265},   // 08-12 15:30Z
    {1755013500000LL, 11.265, 11.3, 11.265, 11.29},   // 08-12 15:45Z
    {1755014400000LL, 11.285, 11.295, 11.255, 11.265},   // 08-12 16:00Z
    {1755015300000LL, 11.265, 11.28, 11.255, 11.265},   // 08-12 16:15Z
    {1755016200000LL, 11.27, 11.28, 11.25, 11.275},   // 08-12 16:30Z
    {1755017100000LL, 11.28, 11.28, 11.27, 11.275},   // 08-12 16:45Z
    {1755018000000LL, 11.275, 11.275, 11.24, 11.245},   // 08-12 17:00Z
    {1755018900000LL, 11.245, 11.255, 11.24, 11.255},   // 08-12 17:15Z
    {1755019800000LL, 11.255, 11.285, 11.25, 11.285},   // 08-12 17:30Z
    {1755020700000LL, 11.285, 11.285, 11.265, 11.285},   // 08-12 17:45Z
    {1755021600000LL, 11.285, 11.29, 11.255, 11.255},   // 08-12 18:00Z
    {1755022500000LL, 11.255, 11.26, 11.25, 11.255},   // 08-12 18:15Z
    {1755023400000LL, 11.255, 11.255, 11.23, 11.235},   // 08-12 18:30Z
    {1755024300000LL, 11.235, 11.245, 11.21, 11.21},   // 08-12 18:45Z
    {1755025200000LL, 11.215, 11.245, 11.215, 11.245},   // 08-12 19:00Z
    {1755026100000LL, 11.25, 11.25, 11.23, 11.235},   // 08-12 19:15Z
    {1755027000000LL, 11.23, 11.25, 11.22, 11.235},   // 08-12 19:30Z
    {1755027900000LL, 11.23, 11.25, 11.2, 11.24},   // 08-12 19:45Z
    {1755091800000LL, 11.29, 11.29, 11.19, 11.25},   // 08-13 13:30Z
    {1755092700000LL, 11.255, 11.325, 11.25, 11.325},   // 08-13 13:45Z
    {1755093600000LL, 11.325, 11.365, 11.32, 11.33},   // 08-13 14:00Z
    {1755094500000LL, 11.335, 11.335, 11.26, 11.285},   // 08-13 14:15Z
    {1755095400000LL, 11.285, 11.34, 11.28, 11.335},   // 08-13 14:30Z
    {1755096300000LL, 11.33, 11.335, 11.3, 11.325},   // 08-13 14:45Z
    {1755097200000LL, 11.33, 11.36, 11.325, 11.355},   // 08-13 15:00Z
    {1755098100000LL, 11.355, 11.415, 11.355, 11.39},   // 08-13 15:15Z
    {1755099000000LL, 11.39, 11.4, 11.375, 11.385},   // 08-13 15:30Z
    {1755099900000LL, 11.385, 11.385, 11.345, 11.37},   // 08-13 15:45Z
    {1755100800000LL, 11.375, 11.42, 11.37, 11.415},   // 08-13 16:00Z
    {1755101700000LL, 11.415, 11.45, 11.415, 11.425},   // 08-13 16:15Z
    {1755102600000LL, 11.425, 11.45, 11.425, 11.44},   // 08-13 16:30Z
    {1755103500000LL, 11.445, 11.45, 11.435, 11.445},   // 08-13 16:45Z
    {1755104400000LL, 11.44, 11.45, 11.41, 11.41},   // 08-13 17:00Z
    {1755105300000LL, 11.415, 11.445, 11.415, 11.425},   // 08-13 17:15Z
    {1755106200000LL, 11.425, 11.43, 11.4, 11.415},   // 08-13 17:30Z
    {1755107100000LL, 11.415, 11.435, 11.415, 11.425},   // 08-13 17:45Z
    {1755108000000LL, 11.425, 11.45, 11.415, 11.415},   // 08-13 18:00Z
    {1755108900000LL, 11.415, 11.44, 11.415, 11.435},   // 08-13 18:15Z
    {1755109800000LL, 11.44, 11.445, 11.42, 11.43},   // 08-13 18:30Z
    {1755110700000LL, 11.43, 11.45, 11.43, 11.435},   // 08-13 18:45Z
    {1755111600000LL, 11.435, 11.455, 11.435, 11.455},   // 08-13 19:00Z
    {1755112500000LL, 11.455, 11.47, 11.455, 11.465},   // 08-13 19:15Z
    {1755113400000LL, 11.465, 11.485, 11.46, 11.475},   // 08-13 19:30Z
    {1755114300000LL, 11.475, 11.48, 11.425, 11.425},   // 08-13 19:45Z
    {1755178200000LL, 11.3, 11.32, 11.215, 11.225},   // 08-14 13:30Z
    {1755179100000LL, 11.225, 11.27, 11.22, 11.265},   // 08-14 13:45Z
    {1755180000000LL, 11.265, 11.3, 11.25, 11.275},   // 08-14 14:00Z
    {1755180900000LL, 11.27, 11.275, 11.25, 11.265},   // 08-14 14:15Z
    {1755181800000LL, 11.265, 11.3, 11.265, 11.29},   // 08-14 14:30Z
    {1755182700000LL, 11.29, 11.315, 11.29, 11.305},   // 08-14 14:45Z
    {1755183600000LL, 11.3, 11.315, 11.295, 11.295},   // 08-14 15:00Z
    {1755184500000LL, 11.3, 11.315, 11.29, 11.305},   // 08-14 15:15Z
    {1755185400000LL, 11.31, 11.325, 11.295, 11.3},   // 08-14 15:30Z
    {1755186300000LL, 11.295, 11.32, 11.27, 11.315},   // 08-14 15:45Z
    {1755187200000LL, 11.31, 11.315, 11.29, 11.305},   // 08-14 16:00Z
    {1755188100000LL, 11.305, 11.31, 11.28, 11.285},   // 08-14 16:15Z
    {1755189000000LL, 11.29, 11.29, 11.275, 11.29},   // 08-14 16:30Z
    {1755189900000LL, 11.285, 11.33, 11.285, 11.325},   // 08-14 16:45Z
    {1755190800000LL, 11.32, 11.325, 11.31, 11.315},   // 08-14 17:00Z
    {1755191700000LL, 11.32, 11.345, 11.315, 11.345},   // 08-14 17:15Z
    {1755192600000LL, 11.345, 11.36, 11.345, 11.36},   // 08-14 17:30Z
    {1755193500000LL, 11.36, 11.37, 11.35, 11.355},   // 08-14 17:45Z
    {1755194400000LL, 11.355, 11.37, 11.355, 11.365},   // 08-14 18:00Z
    {1755195300000LL, 11.36, 11.38, 11.355, 11.36},   // 08-14 18:15Z
    {1755196200000LL, 11.355, 11.385, 11.355, 11.385},   // 08-14 18:30Z
    {1755197100000LL, 11.38, 11.39, 11.37, 11.385},   // 08-14 18:45Z
    {1755198000000LL, 11.38, 11.41, 11.38, 11.405},   // 08-14 19:00Z
    {1755198900000LL, 11.405, 11.42, 11.405, 11.41},   // 08-14 19:15Z
    {1755199800000LL, 11.415, 11.44, 11.41, 11.43},   // 08-14 19:30Z
    {1755200700000LL, 11.435, 11.45, 11.43, 11.435},   // 08-14 19:45Z
    {1755264600000LL, 11.45, 11.51, 11.45, 11.475},   // 08-15 13:30Z
    {1755265500000LL, 11.48, 11.49, 11.43, 11.435},   // 08-15 13:45Z
    {1755266400000LL, 11.43, 11.44, 11.41, 11.42},   // 08-15 14:00Z
    {1755267300000LL, 11.42, 11.44, 11.41, 11.435},   // 08-15 14:15Z
    {1755268200000LL, 11.43, 11.455, 11.425, 11.45},   // 08-15 14:30Z
    {1755269100000LL, 11.455, 11.46, 11.435, 11.45},   // 08-15 14:45Z
    {1755270000000LL, 11.445, 11.445, 11.43, 11.44},   // 08-15 15:00Z
    {1755270900000LL, 11.44, 11.455, 11.43, 11.43},   // 08-15 15:15Z
    {1755271800000LL, 11.435, 11.45, 11.43, 11.435},   // 08-15 15:30Z
    {1755272700000LL, 11.435, 11.45, 11.43, 11.445},   // 08-15 15:45Z
    {1755273600000LL, 11.44, 11.47, 11.44, 11.46},   // 08-15 16:00Z
    {1755274500000LL, 11.465, 11.475, 11.455, 11.475},   // 08-15 16:15Z
    {1755275400000LL, 11.47, 11.49, 11.46, 11.49},   // 08-15 16:30Z
    {1755276300000LL, 11.485, 11.52, 11.485, 11.49},   // 08-15 16:45Z
    {1755277200000LL, 11.485, 11.5, 11.485, 11.495},   // 08-15 17:00Z
    {1755278100000LL, 11.495, 11.495, 11.48, 11.485},   // 08-15 17:15Z
    {1755279000000LL, 11.485, 11.505, 11.485, 11.495},   // 08-15 17:30Z
    {1755279900000LL, 11.495, 11.505, 11.495, 11.505},   // 08-15 17:45Z
    {1755280800000LL, 11.5, 11.505, 11.475, 11.485},   // 08-15 18:00Z
    {1755281700000LL, 11.485, 11.49, 11.47, 11.485},   // 08-15 18:15Z
    {1755282600000LL, 11.485, 11.49, 11.48, 11.485},   // 08-15 18:30Z
    {1755283500000LL, 11.485, 11.485, 11.46, 11.465},   // 08-15 18:45Z
    {1755284400000LL, 11.465, 11.465, 11.44, 11.44},   // 08-15 19:00Z
    {1755285300000LL, 11.44, 11.455, 11.44, 11.455},   // 08-15 19:15Z
    {1755286200000LL, 11.455, 11.47, 11.45, 11.465},   // 08-15 19:30Z
    {1755287100000LL, 11.465, 11.47, 11.425, 11.435},   // 08-15 19:45Z
    {1755523800000LL, 11.41, 11.425, 11.37, 11.41},   // 08-18 13:30Z
    {1755524700000LL, 11.42, 11.46, 11.42, 11.445},   // 08-18 13:45Z
    {1755525600000LL, 11.445, 11.465, 11.44, 11.44},   // 08-18 14:00Z
    {1755526500000LL, 11.445, 11.455, 11.425, 11.425},   // 08-18 14:15Z
    {1755527400000LL, 11.425, 11.45, 11.425, 11.45},   // 08-18 14:30Z
    {1755528300000LL, 11.45, 11.475, 11.435, 11.445},   // 08-18 14:45Z
    {1755529200000LL, 11.445, 11.465, 11.43, 11.435},   // 08-18 15:00Z
    {1755530100000LL, 11.43, 11.465, 11.43, 11.45},   // 08-18 15:15Z
    {1755531000000LL, 11.455, 11.46, 11.45, 11.455},   // 08-18 15:30Z
    {1755531900000LL, 11.455, 11.46, 11.45, 11.455},   // 08-18 15:45Z
    {1755532800000LL, 11.455, 11.46, 11.42, 11.45},   // 08-18 16:00Z
    {1755533700000LL, 11.445, 11.465, 11.445, 11.455},   // 08-18 16:15Z
    {1755534600000LL, 11.455, 11.465, 11.455, 11.455},   // 08-18 16:30Z
    {1755535500000LL, 11.455, 11.46, 11.445, 11.45},   // 08-18 16:45Z
    {1755536400000LL, 11.445, 11.455, 11.435, 11.455},   // 08-18 17:00Z
    {1755537300000LL, 11.455, 11.46, 11.455, 11.455},   // 08-18 17:15Z
    {1755538200000LL, 11.455, 11.47, 11.445, 11.455},   // 08-18 17:30Z
    {1755539100000LL, 11.455, 11.455, 11.425, 11.43},   // 08-18 17:45Z
    {1755540000000LL, 11.43, 11.46, 11.43, 11.455},   // 08-18 18:00Z
    {1755540900000LL, 11.45, 11.46, 11.44, 11.445},   // 08-18 18:15Z
    {1755541800000LL, 11.445, 11.445, 11.435, 11.435},   // 08-18 18:30Z
    {1755542700000LL, 11.435, 11.44, 11.435, 11.435},   // 08-18 18:45Z
    {1755543600000LL, 11.435, 11.44, 11.42, 11.425},   // 08-18 19:00Z
    {1755544500000LL, 11.425, 11.435, 11.42, 11.425},   // 08-18 19:15Z
    {1755545400000LL, 11.425, 11.45, 11.425, 11.445},   // 08-18 19:30Z
    {1755546300000LL, 11.445, 11.46, 11.445, 11.45},   // 08-18 19:45Z
    {1755610200000LL, 11.52, 11.66, 11.5, 11.65},   // 08-19 13:30Z
    {1755611100000LL, 11.645, 11.73, 11.635, 11.645},   // 08-19 13:45Z
    {1755612000000LL, 11.65, 11.68, 11.64, 11.67},   // 08-19 14:00Z
    {1755612900000LL, 11.67, 11.71, 11.67, 11.705},   // 08-19 14:15Z
    {1755613800000LL, 11.71, 11.72, 11.665, 11.675},   // 08-19 14:30Z
    {1755614700000LL, 11.675, 11.71, 11.635, 11.635},   // 08-19 14:45Z
    {1755615600000LL, 11.635, 11.64, 11.62, 11.62},   // 08-19 15:00Z
    {1755616500000LL, 11.625, 11.64, 11.6, 11.635},   // 08-19 15:15Z
    {1755617400000LL, 11.63, 11.645, 11.615, 11.615},   // 08-19 15:30Z
    {1755618300000LL, 11.615, 11.615, 11.56, 11.565},   // 08-19 15:45Z
    {1755619200000LL, 11.57, 11.58, 11.56, 11.57},   // 08-19 16:00Z
    {1755620100000LL, 11.565, 11.58, 11.555, 11.555},   // 08-19 16:15Z
    {1755621000000LL, 11.55, 11.57, 11.54, 11.555},   // 08-19 16:30Z
    {1755621900000LL, 11.555, 11.56, 11.535, 11.555},   // 08-19 16:45Z
    {1755622800000LL, 11.56, 11.56, 11.54, 11.555},   // 08-19 17:00Z
    {1755623700000LL, 11.555, 11.57, 11.55, 11.565},   // 08-19 17:15Z
    {1755624600000LL, 11.565, 11.58, 11.565, 11.575},   // 08-19 17:30Z
    {1755625500000LL, 11.575, 11.58, 11.555, 11.555},   // 08-19 17:45Z
    {1755626400000LL, 11.555, 11.555, 11.515, 11.525},   // 08-19 18:00Z
    {1755627300000LL, 11.525, 11.53, 11.51, 11.515},   // 08-19 18:15Z
    {1755628200000LL, 11.51, 11.52, 11.51, 11.52},   // 08-19 18:30Z
    {1755629100000LL, 11.52, 11.555, 11.52, 11.555},   // 08-19 18:45Z
    {1755630000000LL, 11.555, 11.56, 11.545, 11.555},   // 08-19 19:00Z
    {1755630900000LL, 11.555, 11.575, 11.545, 11.575},   // 08-19 19:15Z
    {1755631800000LL, 11.58, 11.595, 11.575, 11.585},   // 08-19 19:30Z
    {1755632700000LL, 11.585, 11.59, 11.57, 11.59},   // 08-19 19:45Z
    {1755696600000LL, 11.52, 11.58, 11.505, 11.565},   // 08-20 13:30Z
    {1755697500000LL, 11.565, 11.595, 11.515, 11.555},   // 08-20 13:45Z
    {1755698400000LL, 11.555, 11.585, 11.52, 11.52},   // 08-20 14:00Z
    {1755699300000LL, 11.525, 11.53, 11.485, 11.485},   // 08-20 14:15Z
    {1755700200000LL, 11.485, 11.525, 11.48, 11.485},   // 08-20 14:30Z
    {1755701100000LL, 11.485, 11.5, 11.475, 11.485},   // 08-20 14:45Z
    {1755702000000LL, 11.485, 11.52, 11.47, 11.52},   // 08-20 15:00Z
    {1755702900000LL, 11.515, 11.55, 11.51, 11.545},   // 08-20 15:15Z
    {1755703800000LL, 11.54, 11.54, 11.505, 11.52},   // 08-20 15:30Z
    {1755704700000LL, 11.525, 11.525, 11.48, 11.5},   // 08-20 15:45Z
    {1755705600000LL, 11.5, 11.53, 11.485, 11.52},   // 08-20 16:00Z
    {1755706500000LL, 11.515, 11.54, 11.515, 11.525},   // 08-20 16:15Z
    {1755707400000LL, 11.525, 11.53, 11.5, 11.525},   // 08-20 16:30Z
    {1755708300000LL, 11.525, 11.525, 11.5, 11.505},   // 08-20 16:45Z
    {1755709200000LL, 11.505, 11.53, 11.505, 11.525},   // 08-20 17:00Z
    {1755710100000LL, 11.525, 11.55, 11.52, 11.54},   // 08-20 17:15Z
    {1755711000000LL, 11.535, 11.56, 11.535, 11.555},   // 08-20 17:30Z
    {1755711900000LL, 11.56, 11.575, 11.555, 11.56},   // 08-20 17:45Z
    {1755712800000LL, 11.56, 11.565, 11.53, 11.535},   // 08-20 18:00Z
    {1755713700000LL, 11.535, 11.55, 11.525, 11.545},   // 08-20 18:15Z
    {1755714600000LL, 11.55, 11.55, 11.535, 11.54},   // 08-20 18:30Z
    {1755715500000LL, 11.545, 11.55, 11.52, 11.525},   // 08-20 18:45Z
    {1755716400000LL, 11.525, 11.54, 11.52, 11.535},   // 08-20 19:00Z
    {1755717300000LL, 11.535, 11.535, 11.5, 11.505},   // 08-20 19:15Z
    {1755718200000LL, 11.505, 11.52, 11.505, 11.515},   // 08-20 19:30Z
    {1755719100000LL, 11.515, 11.54, 11.49, 11.49},   // 08-20 19:45Z
    {1755783000000LL, 11.42, 11.43, 11.23, 11.24},   // 08-21 13:30Z
    {1755783900000LL, 11.24, 11.3, 11.2, 11.3},   // 08-21 13:45Z
    {1755784800000LL, 11.295, 11.34, 11.29, 11.325},   // 08-21 14:00Z
    {1755785700000LL, 11.33, 11.345, 11.305, 11.305},   // 08-21 14:15Z
    {1755786600000LL, 11.31, 11.31, 11.27, 11.285},   // 08-21 14:30Z
    {1755787500000LL, 11.285, 11.31, 11.28, 11.305},   // 08-21 14:45Z
    {1755788400000LL, 11.305, 11.32, 11.275, 11.275},   // 08-21 15:00Z
    {1755789300000LL, 11.275, 11.3, 11.275, 11.29},   // 08-21 15:15Z
    {1755790200000LL, 11.295, 11.33, 11.29, 11.325},   // 08-21 15:30Z
    {1755791100000LL, 11.325, 11.325, 11.305, 11.305},   // 08-21 15:45Z
    {1755792000000LL, 11.305, 11.345, 11.3, 11.335},   // 08-21 16:00Z
    {1755792900000LL, 11.335, 11.34, 11.32, 11.325},   // 08-21 16:15Z
    {1755793800000LL, 11.325, 11.335, 11.315, 11.325},   // 08-21 16:30Z
    {1755794700000LL, 11.325, 11.335, 11.305, 11.325},   // 08-21 16:45Z
    {1755795600000LL, 11.325, 11.325, 11.305, 11.325},   // 08-21 17:00Z
    {1755796500000LL, 11.325, 11.325, 11.295, 11.31},   // 08-21 17:15Z
    {1755797400000LL, 11.31, 11.35, 11.31, 11.345},   // 08-21 17:30Z
    {1755798300000LL, 11.345, 11.36, 11.345, 11.35},   // 08-21 17:45Z
    {1755799200000LL, 11.35, 11.37, 11.345, 11.37},   // 08-21 18:00Z
    {1755800100000LL, 11.365, 11.375, 11.345, 11.345},   // 08-21 18:15Z
    {1755801000000LL, 11.34, 11.35, 11.335, 11.345},   // 08-21 18:30Z
    {1755801900000LL, 11.345, 11.375, 11.34, 11.375},   // 08-21 18:45Z
    {1755802800000LL, 11.37, 11.375, 11.36, 11.365},   // 08-21 19:00Z
    {1755803700000LL, 11.365, 11.365, 11.34, 11.345},   // 08-21 19:15Z
    {1755804600000LL, 11.345, 11.35, 11.33, 11.335},   // 08-21 19:30Z
    {1755805500000LL, 11.33, 11.35, 11.32, 11.335},   // 08-21 19:45Z
    {1755869400000LL, 11.39, 11.49, 11.39, 11.485},   // 08-22 13:30Z
    {1755870300000LL, 11.485, 11.525, 11.475, 11.495},   // 08-22 13:45Z
    {1755871200000LL, 11.5, 11.71, 11.5, 11.705},   // 08-22 14:00Z
    {1755872100000LL, 11.7, 11.76, 11.655, 11.68},   // 08-22 14:15Z
    {1755873000000LL, 11.68, 11.74, 11.68, 11.73},   // 08-22 14:30Z
    {1755873900000LL, 11.725, 11.77, 11.715, 11.75},   // 08-22 14:45Z
    {1755874800000LL, 11.75, 11.765, 11.705, 11.725},   // 08-22 15:00Z
    {1755875700000LL, 11.72, 11.745, 11.71, 11.725},   // 08-22 15:15Z
    {1755876600000LL, 11.725, 11.74, 11.705, 11.715},   // 08-22 15:30Z
    {1755877500000LL, 11.715, 11.745, 11.71, 11.72},   // 08-22 15:45Z
    {1755878400000LL, 11.72, 11.76, 11.705, 11.735},   // 08-22 16:00Z
    {1755879300000LL, 11.73, 11.755, 11.705, 11.72},   // 08-22 16:15Z
    {1755880200000LL, 11.72, 11.745, 11.72, 11.72},   // 08-22 16:30Z
    {1755881100000LL, 11.725, 11.75, 11.72, 11.73},   // 08-22 16:45Z
    {1755882000000LL, 11.735, 11.765, 11.73, 11.76},   // 08-22 17:00Z
    {1755882900000LL, 11.755, 11.755, 11.73, 11.73},   // 08-22 17:15Z
    {1755883800000LL, 11.74, 11.745, 11.73, 11.735},   // 08-22 17:30Z
    {1755884700000LL, 11.74, 11.75, 11.715, 11.715},   // 08-22 17:45Z
    {1755885600000LL, 11.72, 11.725, 11.7, 11.71},   // 08-22 18:00Z
    {1755886500000LL, 11.71, 11.725, 11.71, 11.715},   // 08-22 18:15Z
    {1755887400000LL, 11.715, 11.725, 11.695, 11.725},   // 08-22 18:30Z
    {1755888300000LL, 11.72, 11.73, 11.71, 11.72},   // 08-22 18:45Z
    {1755889200000LL, 11.715, 11.73, 11.715, 11.72},   // 08-22 19:00Z
    {1755890100000LL, 11.72, 11.73, 11.71, 11.715},   // 08-22 19:15Z
    {1755891000000LL, 11.715, 11.74, 11.715, 11.73},   // 08-22 19:30Z
    {1755891900000LL, 11.73, 11.74, 11.715, 11.73},   // 08-22 19:45Z
};
constexpr int kF0811Count = sizeof(kF0811) / sizeof(kF0811[0]);

std::vector<Bar> f0811_bars() {
    std::vector<Bar> b;
    for (int i = 0; i < kF0811Count; ++i) {
        b.push_back(mk(kF0811[i].ts, kF0811[i].o, kF0811[i].h, kF0811[i].l,
                       kF0811[i].c));
    }
    return b;
}

// NYSE:F 15, 2025-04-03 19:00Z .. 2025-04-04 15:15Z, with the ahtisham
// levels = hand replay of the Pine indicators over the registry feed
// (zoneHigh / zoneLow = ta.highest / ta.lowest of high[1] / low[1] over 20,
// RMA-14 ATR from the feed start, offset 1.5 atr; scratchpad/r7/k/
// aht_model.py). b0..b3 = 04-03 19:00Z..19:45Z, b4.. = 04-04 13:30Z..15:15Z.
struct LvlRow { double o, h, l, c, buy_stop, sell_stop, mid; };
enum BBar {
    B0403_1945 = 3, B0404_1330 = 4, B0404_1345 = 5, B0404_1400 = 6,
    B0404_1500 = 10, B0404_1515 = 11,
};
const LvlRow kAht0404[] = {
    {9.68, 9.685, 9.65, 9.65, 9.9865, 9.5335, 9.7600},   // b0 04-03 19:00Z
    {9.65, 9.65, 9.6, 9.605, 9.9507, 9.5343, 9.7425},   // b1 04-03 19:15Z
    {9.605, 9.625, 9.595, 9.61, 9.9228, 9.5172, 9.7200},   // b2 04-03 19:30Z
    {9.615, 9.615, 9.53, 9.545, 9.9110, 9.5090, 9.7100},   // b3 04-03 19:45Z
    {9.32, 9.39, 9.21, 9.335, 9.9407, 9.4143, 9.6775},   // b4 04-04 13:30Z
    {9.34, 9.435, 9.305, 9.385, 9.9464, 9.0886, 9.5175},   // b5 04-04 13:45Z
    {9.38, 9.44, 9.345, 9.37, 9.9479, 9.0871, 9.5175},   // b6 04-04 14:00Z
    {9.375, 9.42, 9.34, 9.375, 9.9477, 9.0873, 9.5175},   // b7 04-04 14:15Z
    {9.38, 9.42, 9.33, 9.33, 9.9486, 9.0864, 9.5175},   // b8 04-04 14:30Z
    {9.325, 9.395, 9.2, 9.365, 9.9606, 9.0744, 9.5175},   // b9 04-04 14:45Z
    {9.36, 9.58, 9.345, 9.565, 9.9611, 9.0489, 9.5050},   // b10 04-04 15:00Z
    {9.57, 9.66, 9.5, 9.5, 9.9325, 9.0425, 9.4875},   // b11 04-04 15:15Z
};
constexpr int kAht0404Count = sizeof(kAht0404) / sizeof(kAht0404[0]);

std::vector<Bar> aht0404_bars() {
    const int64_t t0403 = 1743706800000LL;   // 2025-04-03 19:00Z
    const int64_t t0404 = 1743773400000LL;   // 2025-04-04 13:30Z
    std::vector<Bar> b;
    for (int i = 0; i < kAht0404Count; ++i) {
        const int64_t ts = i < 4 ? t0403 + i * kMin15 : t0404 + (i - 4) * kMin15;
        b.push_back(mk(ts, kAht0404[i].o, kAht0404[i].h, kAht0404[i].l,
                       kAht0404[i].c));
    }
    return b;
}

class Probe : public pineforge::source::PineStrategyHost {
public:
    // NYSE:F: mintick 0.01, whole shares, Pine v6 defaults (margin 100,
    // pyramiding 0 = one entry, no commission / slippage, margin call ON in
    // TV — enabled per test where the tape shows its slices).
    Probe(double capital, double pct, double margin = 100.0) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = pct;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = margin;
        margin_short_ = margin;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(false);
    }
    std::function<void(Probe&, int)> script;
    void on_source_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }

    double equity() const {
        return current_equity() + open_profit(current_bar_.close);
    }
    double position_size() const { return signed_position_size(); }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    double close_now() const { return current_bar_.close; }
    const PendingOrder* pending(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id == id) return &o;
        return nullptr;
    }
    // Placement record: (bar, id, placed?, placement qty, sizing price).
    struct Placement { int bar; std::string id; bool placed; double qty; double basis; };
    std::vector<Placement> placements;
    void entry_stop(const std::string& id, bool is_long, double level,
                    const std::string& comment = "") {
        strategy_entry(id, is_long, kNaN, level, kNaN, comment);
        const PendingOrder* o = pending(id);
        placements.push_back({bar_index_, id, o != nullptr,
                              o ? o->default_stop_placement_qty : kNaN,
                              o ? o->default_stop_sizing_price : kNaN});
    }
    const Placement* placement(int bar, const std::string& id) const {
        for (const auto& p : placements) {
            if (p.bar == bar && p.id == id) return &p;
        }
        return nullptr;
    }
    bool placed_on(int bar, const std::string& id) const {
        const Placement* p = placement(bar, id);
        return p != nullptr && p->placed;
    }
    int placements_of(const std::string& id) const {
        int n = 0;
        for (const auto& p : placements) if (p.id == id && p.placed) ++n;
        return n;
    }
    int calls_of(const std::string& id) const {
        int n = 0;
        for (const auto& p : placements) if (p.id == id) ++n;
        return n;
    }
    void enable_margin_call() { set_margin_call_enabled(true); }
    using pineforge::source::PineStrategyHost::strategy_entry;
    using pineforge::source::PineStrategyHost::strategy_exit;
    using pineforge::source::PineStrategyHost::strategy_close;
    using pineforge::source::PineStrategyHost::strategy_close_all;
};

// The four tapes' script: while flat a buy stop 0.20 above and/or a sell
// stop 0.20 below the close, re-issued every bar; strategy.close_all the bar
// after an entry (fills at the next open).
void tape_script(Probe& e, bool longs, bool shorts) {
    if (e.position_size() == 0) {
        if (longs) e.entry_stop("L", true, e.close_now() + 0.20, "L");
        if (shorts) e.entry_stop("S", false, e.close_now() - 0.20, "S");
    } else {
        e.strategy_close_all();
    }
}

struct ExpectedTrade {
    bool is_long; int entry_bar; double entry_price; double qty;
    int exit_bar; double exit_price; double pnl;
};

void check_trades(const Probe& p, const std::vector<ExpectedTrade>& expected) {
    CHECK(p.trade_count() == (int)expected.size());
    for (size_t i = 0; i < expected.size() && (int)i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade((int)i);
        const ExpectedTrade& x = expected[i];
        CHECK(t.is_long == x.is_long);
        CHECK(t.entry_bar_index == x.entry_bar);
        CHECK_NEAR(t.entry_price, x.entry_price, 1e-9);
        CHECK_NEAR(t.qty, x.qty, 1e-9);
        CHECK(t.exit_bar_index == x.exit_bar);
        CHECK_NEAR(t.exit_price, x.exit_price, 1e-9);
        CHECK_NEAR(t.pnl, x.pnl, 1e-6);
    }
}

// --- tape f15-stopsize-pct100 (pct 100, margin 100, both sides) ---
// TV: 2 trades, both LONG touches — 08-19 13:30Z L 858 @11.65 (placed at the
// 08-18 19:45Z close 11.45: level 11.65, 858 = floor(10,000 / 11.65); the
// close would size 873), out 13:45Z @11.65; 08-22 14:00Z L 854 @11.70 (close
// 11.50 -> level 11.70; 869 at the close), out 14:15Z @11.70. The sell stop
// 0.20 below the close is NEVER placed (floor(eq / L) x tick(close) > eq on
// every one of the 258 flat closes) although its level is touched three
// times (08-11 14:30Z l 11.06 < 11.10, 08-14 13:30Z, 08-21 13:30Z).
void test_pct100_tape() {
    std::printf("-- pct100: longs sized at the level (858 / 854), the all-in sell stop below the close is never placed --\n");
    Probe p(10000.0, 100.0);
    p.script = [&](Probe& e, int) { tape_script(e, true, true); };
    std::vector<Bar> bars = f0811_bars();
    p.run(bars.data(), (int)bars.size());

    // q858: the placement snapshot on the 08-18 19:45Z close.
    const Probe::Placement* l = p.placement(A0818_1945, "L");
    CHECK(l != nullptr && l->placed);
    if (l != nullptr) {
        CHECK_NEAR(l->qty, 858.0, 1e-9);
        CHECK_NEAR(l->basis, 11.65, 1e-9);
    }
    const Probe::Placement* l2 = p.placement(A0822_1400 - 1, "L");
    CHECK(l2 != nullptr && l2->placed);
    if (l2 != nullptr) CHECK_NEAR(l2->qty, 854.0, 1e-9);

    // The sell stop: called on every flat bar, placed on none.
    CHECK(p.calls_of("S") > 250);
    CHECK(p.placements_of("S") == 0);
    CHECK(!p.placed_on(A0811_1415, "S"));   // touched 14:30Z (l 11.06 < 11.10)
    CHECK(!p.placed_on(A0813_1945, "S"));   // touched 08-14 13:30Z
    CHECK(!p.placed_on(A0820_1945, "S"));   // touched 08-21 13:30Z
    const Probe::Placement* s = p.placement(A0818_1945, "S");
    CHECK(s != nullptr && !s->placed);      // 11.25: 888 x 11.45 = 10,167.6 > 10,000

    check_trades(p, {
        {true, A0819_1330, 11.65, 858.0, A0819_1345, 11.65, 0.0},
        {true, A0822_1400, 11.70, 854.0, A0822_1415, 11.70, 0.0},
    });
    CHECK(p.flat());
}

// --- tape f15-stopsize-short-only (pct 100, sell stop only) ---
// TV: 0 trades. With no long order pending the result is identical, so the
// never-placed short is not an OCA / opposite-order effect.
void test_short_only_tape() {
    std::printf("-- short-only: pct 100 sell stop below the close, no long pending: never placed, 0 trades --\n");
    Probe p(10000.0, 100.0);
    p.script = [&](Probe& e, int) { tape_script(e, false, true); };
    std::vector<Bar> bars = f0811_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.trade_count() == 0);
    CHECK(p.calls_of("S") == kF0811Count);
    CHECK(p.placements_of("S") == 0);
    CHECK(p.flat());
}

// --- tape f15-stopsize-pct50 (pct 50, both sides) ---
// TV: 5 trades. Shorts place (floor(0.5 eq / L) x C <= eq) and fill at the
// level: 08-11 14:30Z S 450 @11.09 (450 = floor(5,000 / 11.09); 442 at the
// close 11.29) out 14:45Z @11.15 (-27); 08-14 13:30Z S 444 @11.22 (floor(0.5
// x 9,973 / 11.22)) out @11.23 (-4.44); 08-19 13:30Z L 427 @11.65 out @11.65;
// 08-21 13:30Z S 441 @11.29 out @11.24 (+22.05); 08-22 14:00Z L 426 @11.70
// out @11.70.
void test_pct50_tape() {
    std::printf("-- pct50: shorts placed and filled at the level, 450 / 444 / 427 / 441 / 426 --\n");
    Probe p(10000.0, 50.0);
    p.script = [&](Probe& e, int) { tape_script(e, true, true); };
    std::vector<Bar> bars = f0811_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(A0811_1415, "S"));
    const Probe::Placement* s = p.placement(A0811_1415, "S");
    if (s != nullptr) {
        CHECK_NEAR(s->qty, 450.0, 1e-9);
        CHECK_NEAR(s->basis, 11.09, 1e-9);
    }
    check_trades(p, {
        {false, A0811_1430, 11.09, 450.0, A0811_1445, 11.15, -27.0},
        {false, A0814_1330, 11.22, 444.0, A0814_1345, 11.23, -4.44},
        {true,  A0819_1330, 11.65, 427.0, A0819_1345, 11.65, 0.0},
        {false, A0821_1330, 11.29, 441.0, A0821_1345, 11.24, 22.05},
        {true,  A0822_1400, 11.70, 426.0, A0822_1415, 11.70, 0.0},
    });
    CHECK(p.flat());
}

// --- tape f15-stopsize-short-m50 (pct 100, margin 50, sell stop only) ---
// TV: 3 short touch fills sized floor(eq / L) — the margin halves the
// placement cost (floor(eq/L) x C x 0.5 <= eq): 08-11 14:30Z 901 @11.09 out
// @11.15 (-54.06); 08-14 13:30Z 886 @11.22 out @11.23 (-8.86); 08-21 13:30Z
// 880 @11.29 out @11.24 (+44).
void test_short_m50_tape() {
    std::printf("-- short-m50: margin 50 places the all-in sell stop, fills 901 / 886 / 880 at the level --\n");
    Probe p(10000.0, 100.0, 50.0);
    p.script = [&](Probe& e, int) { tape_script(e, false, true); };
    std::vector<Bar> bars = f0811_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(A0811_1415, "S"));
    check_trades(p, {
        {false, A0811_1430, 11.09, 901.0, A0811_1445, 11.15, -54.06},
        {false, A0814_1330, 11.22, 886.0, A0814_1345, 11.23, -8.86},
        {false, A0821_1330, 11.29, 880.0, A0821_1345, 11.24, 44.0},
    });
    CHECK(p.flat());
}

// --- ahtisham F@15 2025-08-19 13:30Z: the first-bar LONG gap-through TV fills ---
// Equity 9,414.16 (TV cumulative before the trade), buyStopLevel 11.5069 at
// the 08-18 19:45Z close 11.45 -> level 11.51, qty 817 = floor(9,414.16 /
// 11.51). 08-19 opens 11.52 through the level: fill at the rounded open,
// 817 x 11.52 = 9,411.84 <= 9,414.16 admitted — TV's q817 @11.52. Sized at
// the close (822) the same fill costs 9,469.44 and is declined (the engine's
// 0/19 before this change).
void test_0819_long_gap_through_fills_817() {
    std::printf("-- 08-19 13:30Z long gap-through: 817 = floor(eq / 11.51) x 11.52 admitted --\n");
    Probe p(9414.16, 100.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == A0818_1945) e.entry_stop("Long", true, 11.5069, "EXPANSION UP");
        if (bar == A0819_1345 && e.position_size() > 0) e.strategy_close_all();
    };
    std::vector<Bar> bars = f0811_bars();
    p.run(bars.data(), (int)bars.size());
    CHECK(p.placed_on(A0818_1945, "Long"));
    const Probe::Placement* l = p.placement(A0818_1945, "Long");
    if (l != nullptr) {
        CHECK_NEAR(l->qty, 817.0, 1e-9);
        CHECK_NEAR(l->basis, 11.51, 1e-9);
    }
    CHECK(p.trade_count() == 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(t.is_long);
        CHECK(t.entry_bar_index == A0819_1330);
        CHECK_NEAR(t.entry_price, 11.52, 1e-9);
        CHECK_NEAR(t.qty, 817.0, 1e-9);
        CHECK(t.entry_comment == "EXPANSION UP");
    }
}

// --- ahtisham F@15 2025-08-21 13:30Z: a first-bar SHORT gap-through is NOT filled ---
// Equity 9,451.56, sellStopLevel 11.4225 at the 08-20 19:45Z close 11.49 ->
// level 11.42, qty 827 = floor(9,451.56 / 11.42); 827 x 11.49 = 9,502.23 >
// 9,451.56: the placement is rejected and nothing rests, so the 08-21 open
// 11.42 through the level fills nothing (TV NOFILL; the engine filled 822
// @11.42 here before this change).
void test_0821_short_gap_through_not_filled() {
    std::printf("-- 08-21 13:30Z first-bar short gap-through: never placed, no fill --\n");
    Probe p(9451.56, 100.0);
    p.script = [&](Probe& e, int bar) {
        if (bar == A0820_1945) e.entry_stop("Short", false, 11.4225, "EXPANSION DOWN");
    };
    std::vector<Bar> bars = f0811_bars();
    p.run(bars.data(), (int)bars.size());
    const Probe::Placement* s = p.placement(A0820_1945, "Short");
    CHECK(s != nullptr && !s->placed);
    CHECK(p.trade_count() == 0);
    CHECK(p.flat());
}

// --- ahtisham F@15 first divergence: 2025-04-03 19:45Z .. 04-04 15:15Z ---
// TV equity 9,742.34 after trade 1. At the 04-03 19:45Z close 9.545 (-> 9.55)
// the sell stop 9.5090 -> 9.50 sizes 1,025 and 1,025 x 9.55 = 9,788.75 >
// 9,742.34: not placed; the buy stop 9.9110 -> 9.92 (982) is. 04-04 13:30Z
// gaps down to 9.32 through 9.50: NOTHING fills (the engine filled 1,020
// @9.32 here before this change — its first divergence on this probe). At
// the 13:30Z close 9.335 (-> 9.34) the sell stop 9.4143 -> 9.41 is already
// beyond the close: a market order sized at tick(close), 1,043 = floor(
// 9,742.34 / 9.34), filling at the 13:45Z open 9.34 (1,043 x 9.34 = 9,741.62
// <= 9,742.34). TV's tape: trade 2 = 88 @9.34 margin-called 13:45Z @9.44,
// trade 3 = 955 @9.34 stopped 15:00Z @9.52 ("Fakeout", the 9.5175 mid ->
// 9.52 buy stop).
void aht_script(Probe& e, int bar, bool with_exits) {
    const LvlRow& r = kAht0404[bar];
    if (e.position_size() == 0) {
        e.entry_stop("Long", true, r.buy_stop, "EXPANSION UP");
        e.entry_stop("Short", false, r.sell_stop, "EXPANSION DOWN");
    }
    if (!with_exits) return;
    if (e.position_size() > 0) {
        const double tp = r.buy_stop + std::fabs(r.buy_stop - r.mid) * 2.0;
        e.strategy_exit("L-Exit", "Long", tp, r.mid);
    }
    if (e.position_size() < 0) {
        const double tp = r.sell_stop - std::fabs(r.sell_stop - r.mid) * 2.0;
        e.strategy_exit("S-Exit", "Short", tp, r.mid);
    }
}

void test_ahtisham_0404_first_divergence() {
    std::printf("-- ahtisham 04-04: no gap fill at 13:30Z, the beyond-level short is market-sized 1,043 at the 13:45Z open --\n");
    Probe p(9742.34, 100.0);
    p.script = [&](Probe& e, int bar) {
        aht_script(e, bar, /*with_exits=*/true);
        if (bar == B0404_1330) {
            // The bar that gapped through the never-placed 9.50 sell stop.
            CHECK(e.flat());
            CHECK(e.trade_count() == 0);
        }
    };
    std::vector<Bar> bars = aht0404_bars();
    p.run(bars.data(), (int)bars.size());

    // 04-03 19:45Z: the sell stop is rejected at placement, the buy stop rests.
    const Probe::Placement* s0 = p.placement(B0403_1945, "Short");
    CHECK(s0 != nullptr && !s0->placed);
    const Probe::Placement* l0 = p.placement(B0403_1945, "Long");
    CHECK(l0 != nullptr && l0->placed);
    if (l0 != nullptr) {
        CHECK_NEAR(l0->qty, 982.0, 1e-9);        // floor(9,742.34 / 9.92)
        CHECK_NEAR(l0->basis, 9.92, 1e-9);
    }
    // 04-04 13:30Z close: the sell stop 9.41 is beyond the 9.34 close ->
    // sized at tick(close), not at the level (1,035) nor at the open (1,045).
    const Probe::Placement* s1 = p.placement(B0404_1330, "Short");
    CHECK(s1 != nullptr && s1->placed);
    if (s1 != nullptr) {
        CHECK_NEAR(s1->qty, 1043.0, 1e-9);
        CHECK_NEAR(s1->basis, 9.34, 1e-9);
    }
    // 13:45Z: short 1,043 @9.34; stopped 15:00Z @9.52 (margin call off here:
    // one trade carries the whole lot).
    CHECK(p.trade_count() == 1);
    if (p.trade_count() >= 1) {
        const Trade& t = p.get_trade(0);
        CHECK(!t.is_long);
        CHECK(t.entry_bar_index == B0404_1345);
        CHECK_NEAR(t.entry_price, 9.34, 1e-9);
        CHECK_NEAR(t.qty, 1043.0, 1e-9);
        CHECK(t.entry_comment == "EXPANSION DOWN");
        CHECK(t.exit_bar_index == B0404_1500);
        CHECK_NEAR(t.exit_price, 9.52, 1e-9);
    }
}

// The same sequence with TV's margin call on: the 13:45Z bar (h 9.435 ->
// 9.44) slices the under-margined lot — TV's trade 2, 88 @9.34 -> @9.44 —
// and the remaining 955 are stopped 15:00Z @9.52 (trade 3). The entries
// still sum to the 1,043 sized at tick(close).
void test_ahtisham_0404_margin_call_slices() {
    std::printf("-- ahtisham 04-04 with margin call: 88 sliced @9.44 on the fill bar, 955 stopped @9.52 --\n");
    Probe p(9742.34, 100.0);
    p.enable_margin_call();
    p.script = [&](Probe& e, int bar) { aht_script(e, bar, /*with_exits=*/true); };
    std::vector<Bar> bars = aht0404_bars();
    p.run(bars.data(), (int)bars.size());
    double entered = 0.0;
    bool all_short_at_0345 = p.trade_count() > 0;
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        entered += t.qty;
        if (t.is_long || t.entry_bar_index != B0404_1345
            || std::fabs(t.entry_price - 9.34) > 1e-9) {
            all_short_at_0345 = false;
        }
    }
    CHECK(all_short_at_0345);
    CHECK_NEAR(entered, 1043.0, 1e-9);
    CHECK(p.trade_count() == 2);
    if (p.trade_count() == 2) {
        const Trade& mc = p.get_trade(0);
        CHECK_NEAR(mc.qty, 88.0, 1e-9);
        CHECK(mc.exit_bar_index == B0404_1345);
        CHECK_NEAR(mc.exit_price, 9.44, 1e-9);
        const Trade& rest = p.get_trade(1);
        CHECK_NEAR(rest.qty, 955.0, 1e-9);
        CHECK(rest.exit_bar_index == B0404_1500);
        CHECK_NEAR(rest.exit_price, 9.52, 1e-9);
    }
}

// --- rule 2 on a default stop: a rejected same-id re-issue cancels the resting order ---
// (family E, xau-flatten-replace-c10983; the K pin: "a rejected placement is
// dropped and only the script's next call re-issues it".) Synthetic bars,
// mintick 0.01, whole shares, pct 100: close 11.44, sell stop 11.43 -> 874 =
// floor(10,000 / 11.43), 874 x 11.44 = 9,998.56 <= 10,000 PLACED (an all-in
// sell stop one tick below the close can pass when the lot floor absorbs
// the tick). Bar 1 (no touch) closes 11.60: the re-issue at the same level
// costs 874 x 11.60 = 10,138.4 > 10,000 -> rejected AND the resting 874 is
// cancelled; bar 2 gaps through the level (o 11.30) and fills nothing.
// Armed once (no re-issue) the resting order fills the gap: 874 @11.30
// (874 x 11.30 = 9,876.2 <= 10,000), the placement quantity, not the 884 a
// fill-time re-size at 11.30 would open.
void test_rejected_reissue_cancels_resting_default_stop() {
    std::printf("-- rejected same-id re-issue cancels the resting default stop; armed once it fills the gap with its placement qty --\n");
    std::vector<Bar> bars = {
        mk(1000, 11.40, 11.45, 11.38, 11.44),
        mk(2000, 11.50, 11.60, 11.45, 11.60),
        mk(3000, 11.30, 11.35, 11.25, 11.32),
        mk(4000, 11.32, 11.33, 11.31, 11.32),
    };
    for (bool reissue : {true, false}) {
        Probe p(10000.0, 100.0);
        p.script = [&](Probe& e, int bar) {
            if (bar == 0 || (reissue && bar == 1)) {
                e.entry_stop("S", false, 11.43, "S");
            }
        };
        p.run(bars.data(), (int)bars.size());
        CHECK(p.placed_on(0, "S"));
        const Probe::Placement* s = p.placement(0, "S");
        if (s != nullptr) {
            CHECK_NEAR(s->qty, 874.0, 1e-9);
            CHECK_NEAR(s->basis, 11.43, 1e-9);
        }
        if (reissue) {
            CHECK(!p.placed_on(1, "S"));
            CHECK(p.pending("S") == nullptr);
            CHECK(p.flat());
            CHECK(p.trade_count() == 0);
        } else {
            CHECK(!p.flat());
            CHECK(p.position_size() < 0);
            CHECK_NEAR(-p.position_size(), 874.0, 1e-9);
        }
    }
}

}  // namespace

int main() {
    std::printf("--- default_pct_stop_sizing (round 7 family K, log-20260905t084529z-c7b22df1) ---\n");
    test_pct100_tape();
    test_short_only_tape();
    test_pct50_tape();
    test_short_m50_tape();
    test_0819_long_gap_through_fills_817();
    test_0821_short_gap_through_not_filled();
    test_ahtisham_0404_first_divergence();
    test_ahtisham_0404_margin_call_slices();
    test_rejected_reissue_cancels_resting_default_stop();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
