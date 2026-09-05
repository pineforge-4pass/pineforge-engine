/*
 * test_aapl15_margin_brackets.cpp — round 7 family N: the NASDAQ:AAPL 15m
 * near-miss singletons (algoai, shojiy, willowsportz, fast-scalper,
 * therealbouga) — three engine mechanisms, all replayed on the registry's own
 * NASDAQ:AAPL 15 bars (feed ae2b03d3736f) and NYSE:F 15 bars (feed
 * 80f404ae85ef) with the tapes' capital and orders.
 *
 * M1 — campaign pin log-20260905t112243z-b6ddd126 (lab tv tape
 *   scratchpad/r7/pins/aapl15-mcopen-willow): a forced-liquidation slice on a
 *   bar whose OPEN already breaches margin is sized with the position MARKED
 *   AT THE TICK-ROUNDED OPEN — slice = max(1, 4 x floor(x)),
 *   x = (q x P - E(P)) / P with P = round_to_mintick(open) — the same
 *   on-tick ledger the adverse-extreme cascade marks on. The engine marked
 *   the raw half-tick open (196.135 -> 408 where TV prints 412 at 196.14).
 *   A. willowsportz 04-22 13:30Z: 412 @196.14 (the whole tape row-for-row:
 *      12 / 36 / 156 / 412 / 676 / 4165).
 *   B. algoai 06-20 13:30Z (o 198.235): 64 @198.24, then the 'Short Exit'
 *      stop 3803 @200.00 on the same bar (the engine printed 60 / 3807).
 *
 * M2 — note log-20260905t112259z-33f32db4: on a bar whose OPEN carries a
 *   declined all-in reversal, TradingView's sequence is decline -> bracket
 *   dormant -> margin slice -> REVIVE, so the resting stop is live again for
 *   the rest of the bar; at an adverse-extreme cascade a revived marketable
 *   bracket closes the remainder AT THE SLICE PRICE on the same bar.
 *   C. algoai 10-30 13:30Z (lab tv tape aapl15-mcopen1-stop-algoai + the
 *      probe's declined ema9/21 reversal): 1 @271.96 open slice, then the
 *      'X' stop 2814 @273.69 AT ITS LEVEL (the engine left the bracket
 *      dormant: 176 @274.11 and a next-bar close).
 *   D. fast-scalper 07-21 13:30Z (probe rows TV#160/161): the declined
 *      reversal keeps the 213.08 stop dormant across the O->L->H path; the
 *      high 214.86 breaches -> 268 @214.86 'Margin call' AND the revived,
 *      now-marketable stop closes 4621 @214.86 on the same bar (the engine
 *      closed the remainder next bar @214.68: REVIVE-B skipped a re-issued
 *      bracket carrying a frozen full-position qty).
 *   E. control (lab tv tape aapl15-mcext-stop-scalper-b, no reversal): the
 *      stop fills at its level 212.83 x4883, no 07-21 slice — 1 / 20 / 108 /
 *      4883 row-for-row.
 *
 * M3 — note log-20260905t112315z-a234f071 (census 51/51 AAPL + 56/56 F
 *   therealbouga entries, 0 exceptions): layered strategy.exit legs from one
 *   entry — 'TP1' qty_percent=50 + the default 'TP2' (limit+stop) — split
 *   EXACTLY 50/50, bound ONCE at the fill and unchanged by the per-bar
 *   re-issues (strategy.entry re-issued too, refused by pyramiding=0) and by
 *   which leg fires first. The engine lost the split whenever the legs were
 *   armed on a REVERSAL bar: the partial froze against the OLD position
 *   (~25% shape: 125/364 of 489) and a later re-issue then dropped it behind
 *   the still-deferred 100% sibling (0% shape: 502 'S TP2').
 *   F. therealbouga AAPL 05-07 13:30Z: long 236 -> short 502, re-issued
 *      every bar; the 'S TP2' stop at 19:30Z closes 251 @196.10, 251 held.
 *   G. therealbouga AAPL 06-24 14:30Z: short 250 -> long 490 (no re-issue):
 *      'L TP1' 245 @203.26; the 06-25 reversal closes the other 245 @201.41
 *      and its own 'S TP2' stop then closes 245 @202.61.
 *   H. therealbouga F 08-08 13:45Z: short 4591 -> long 8890: 4445 @11.43 +
 *      4445 @11.53 on 08-11 13:30Z; the flat-open 08-11 entry keeps its
 *      4349/4349 split (the stop at 11.26 closes exactly half).
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
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

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct BarRow {
    int64_t ts;
    double open, high, low, close;
};

template <size_t N>
std::vector<Bar> to_bars(const BarRow (&rows)[N]) {
    std::vector<Bar> out;
    out.reserve(N);
    for (const BarRow& r : rows) {
        Bar b;
        b.timestamp = r.ts;
        b.open = r.open; b.high = r.high; b.low = r.low; b.close = r.close;
        b.volume = 1.0;
        out.push_back(b);
    }
    return out;
}

// NASDAQ:AAPL 15 (feed ae2b03d3736f), 2025-04-21 17:45Z .. 2025-04-23 16:15Z.
static const BarRow kAaplWillow[] = {
    {1745257500000LL, 190.61, 190.7, 190.3, 190.515},   // [0] 04-21 17:45 signal
    {1745258400000LL, 190.52, 190.6, 190.25, 190.55},   // [1] 18:00 entry bar
    {1745259300000LL, 190.55, 190.91, 190.53, 190.81},  // [2] 18:15
    {1745260200000LL, 190.81, 191.14, 190.53, 190.59},  // [3] 18:30
    {1745261100000LL, 190.61, 190.66, 190.18, 190.28},  // [4] 18:45
    {1745262000000LL, 190.27, 190.91, 190.25, 190.81},  // [5] 19:00
    {1745262900000LL, 190.81, 191.38, 190.605, 191.1},  // [6] 19:15
    {1745263800000LL, 191.13, 192.09, 191.06, 191.92},  // [7] 19:30
    {1745264700000LL, 191.91, 193.43, 191.61, 193.03},  // [8] 19:45
    {1745328600000LL, 196.135, 197.5, 195.96, 197.25},  // [9] 04-22 13:30 half-tick open
    {1745329500000LL, 197.28, 197.855, 197.14, 197.81}, // [10] 13:45
    {1745330400000LL, 197.87, 198.93, 197.65, 198.19},  // [11] 14:00
    {1745331300000LL, 198.16, 198.26, 197.68, 198.12},  // [12] 14:15
    {1745332200000LL, 198.1, 198.8, 197.68, 198.51},    // [13] 14:30
    {1745333100000LL, 198.49, 199.39, 198.32, 199.3},   // [14] 14:45
    {1745334000000LL, 199.31, 199.46, 198.92, 199.39},  // [15] 15:00
    {1745334900000LL, 199.4, 199.54, 198.8, 198.83},    // [16] 15:15
    {1745335800000LL, 198.825, 198.86, 197.87, 198.03}, // [17] 15:30
    {1745336700000LL, 198.02, 199.38, 197.98, 199.34},  // [18] 15:45
    {1745337600000LL, 199.36, 200.22, 199.02, 200.04},  // [19] 16:00
    {1745338500000LL, 200.01, 201.55, 199.75, 201.39},  // [20] 16:15
    {1745339400000LL, 201.4, 201.58, 200.44, 200.92},   // [21] 16:30
    {1745340300000LL, 200.89, 201.09, 200.29, 200.59},  // [22] 16:45
    {1745341200000LL, 200.61, 201.01, 199.52, 199.68},  // [23] 17:00
    {1745342100000LL, 199.7, 200.17, 198.33, 198.53},   // [24] 17:15
    {1745343000000LL, 198.525, 199.26, 198.17, 198.82}, // [25] 17:30
    {1745343900000LL, 198.82, 198.87, 198.11, 198.27},  // [26] 17:45
    {1745344800000LL, 198.19, 199.37, 198.14, 199.33},  // [27] 18:00
    {1745345700000LL, 199.32, 200.02, 198.97, 199.88},  // [28] 18:15
    {1745346600000LL, 199.9, 200.14, 199.8, 200},       // [29] 18:30
    {1745347500000LL, 200.02, 200.54, 200.02, 200.15},  // [30] 18:45
    {1745348400000LL, 200.16, 200.42, 199.49, 199.57},  // [31] 19:00
    {1745349300000LL, 199.6, 199.63, 198.57, 198.61},   // [32] 19:15
    {1745350200000LL, 198.6, 199.11, 198, 198.92},      // [33] 19:30
    {1745351100000LL, 198.88, 199.89, 198.69, 199.57},  // [34] 19:45
    {1745415000000LL, 206, 207.5, 204.64, 206.7},       // [35] 04-23 13:30 open slice 676
    {1745415900000LL, 206.68, 207.62, 205.86, 207.36},  // [36] 13:45
    {1745416800000LL, 207.38, 208, 205.74, 206.39},     // [37] 14:00
    {1745417700000LL, 206.38, 207.1, 205.63, 206.8},    // [38] 14:15
    {1745418600000LL, 206.75, 207.95, 206.68, 207.73},  // [39] 14:30
    {1745419500000LL, 207.7, 207.77, 206.71, 206.905},  // [40] 14:45
    {1745420400000LL, 206.93, 207.56, 206.19, 206.82},  // [41] 15:00
    {1745421300000LL, 206.83, 206.85, 204.05, 204.62},  // [42] 15:15
    {1745422200000LL, 204.67, 205.42, 204.17, 204.84},  // [43] 15:30
    {1745423100000LL, 204.88, 205.17, 203.67, 204.29},  // [44] 15:45
    {1745424000000LL, 204.26, 204.36, 203.59, 203.81},  // [45] 16:00 close_all
    {1745424900000LL, 203.81, 204.18, 202.79, 204.11},  // [46] 16:15 fill
};

// NASDAQ:AAPL 15, 2025-06-18 19:15Z .. 2025-06-20 14:00Z (06-19 closed).
static const BarRow kAaplAlgoai0620[] = {
    {1750274100000LL, 195.96, 196.29, 195.6, 195.64},   // [0] 06-18 19:15
    {1750275000000LL, 195.635, 196.3, 195.47, 196.3},   // [1] 19:30 signal
    {1750275900000LL, 196.29, 197.11, 196.07, 196.26},  // [2] 19:45 entry bar
    {1750426200000LL, 198.235, 200.94, 197.52, 200.61}, // [3] 06-20 13:30 half-tick open
    {1750427100000LL, 200.62, 200.715, 199.73, 199.85}, // [4] 13:45
    {1750428000000LL, 199.83, 199.93, 198.98, 199.55},  // [5] 14:00
};

// NASDAQ:AAPL 15, 2025-10-29 19:00Z .. 2025-10-30 14:15Z.
static const BarRow kAaplAlgoai1030[] = {
    {1761764400000LL, 269.52, 269.62, 268.28, 268.64},  // [0] 10-29 19:00
    {1761765300000LL, 268.65, 268.96, 268.3, 268.32},   // [1] 19:15 signal
    {1761766200000LL, 268.27, 269.2, 267.8, 269.2},     // [2] 19:30 entry bar
    {1761767100000LL, 269.21, 270.38, 269.05, 269.84},  // [3] 19:45 reversal signal
    {1761831000000LL, 271.96, 274.11, 270.61, 271.21},  // [4] 10-30 13:30 gap open
    {1761831900000LL, 271.18, 271.86, 270.84, 271.075}, // [5] 13:45
    {1761832800000LL, 271.08, 271.37, 270.01, 270.3},   // [6] 14:00
    {1761833700000LL, 270.3, 270.5, 268.99, 269.08},    // [7] 14:15
};

// NASDAQ:AAPL 15, 2025-07-17 19:15Z .. 2025-07-21 14:00Z.
static const BarRow kAaplScalper[] = {
    {1752779700000LL, 210.825, 211.06, 210.825, 210.99}, // [0] 07-17 19:15
    {1752780600000LL, 211, 211.05, 210.68, 210.72},      // [1] 19:30 signal
    {1752781500000LL, 210.71, 210.75, 209.74, 210.02},   // [2] 19:45 entry bar
    {1752845400000LL, 210.87, 211.01, 209.9, 210.03},    // [3] 07-18 13:30
    {1752846300000LL, 210.01, 210.51, 209.89, 210.32},   // [4] 13:45
    {1752847200000LL, 210.33, 210.62, 209.71, 210.1},    // [5] 14:00
    {1752848100000LL, 210.11, 210.31, 209.89, 209.95},   // [6] 14:15
    {1752849000000LL, 209.96, 210.51, 209.78, 210.29},   // [7] 14:30
    {1752849900000LL, 210.34, 211.01, 210.27, 210.49},   // [8] 14:45
    {1752850800000LL, 210.5, 211, 210.44, 210.77},       // [9] 15:00
    {1752851700000LL, 210.74, 210.9, 210.42, 210.83},    // [10] 15:15
    {1752852600000LL, 210.87, 211.005, 210.7, 210.97},   // [11] 15:30
    {1752853500000LL, 210.97, 211.1, 210.93, 211.08},    // [12] 15:45
    {1752854400000LL, 211.07, 211.13, 210.9, 210.94},    // [13] 16:00
    {1752855300000LL, 210.92, 211.105, 210.67, 211},     // [14] 16:15
    {1752856200000LL, 211.02, 211.76, 210.88, 211.64},   // [15] 16:30
    {1752857100000LL, 211.66, 211.79, 211.2, 211.32},    // [16] 16:45
    {1752858000000LL, 211.31, 211.4, 211.05, 211.22},    // [17] 17:00
    {1752858900000LL, 211.25, 211.43, 211.1, 211.19},    // [18] 17:15
    {1752859800000LL, 211.18, 211.53, 211.02, 211.32},   // [19] 17:30
    {1752860700000LL, 211.33, 211.44, 210.97, 211.095},  // [20] 17:45
    {1752861600000LL, 211.1, 211.26, 210.88, 210.93},    // [21] 18:00
    {1752862500000LL, 210.95, 210.97, 210.765, 210.94},  // [22] 18:15
    {1752863400000LL, 210.93, 211.06, 210.86, 211.01},   // [23] 18:30
    {1752864300000LL, 211.01, 211.055, 210.79, 210.97},  // [24] 18:45 stop re-issue
    {1752865200000LL, 210.96, 211.04, 210.88, 211.02},   // [25] 19:00
    {1752866100000LL, 211.02, 211.195, 210.895, 210.95}, // [26] 19:15
    {1752867000000LL, 210.94, 211.065, 210.84, 210.95},  // [27] 19:30
    {1752867900000LL, 210.96, 211.35, 210.835, 211.225}, // [28] 19:45 reversal signal
    {1753104600000LL, 212.06, 214.86, 211.63, 214.67},   // [29] 07-21 13:30
    {1753105500000LL, 214.68, 215.78, 213.96, 214.01},   // [30] 13:45
    {1753106400000LL, 214.05, 214.76, 214.01, 214.73},   // [31] 14:00
};

// NASDAQ:AAPL 15, 2025-05-06 19:00Z .. 2025-05-08 13:45Z.
static const BarRow kAaplBouga0507[] = {
    {1746558000000LL, 199.83, 199.84, 199.23, 199.56},  // [0] 05-06 19:00
    {1746558900000LL, 199.56, 199.805, 199.335, 199.71}, // [1] 19:15
    {1746559800000LL, 199.75, 200.01, 199.58, 199.88},  // [2] 19:30 long signal
    {1746560700000LL, 199.88, 200.16, 198.37, 198.445}, // [3] 19:45 long fill
    {1746624600000LL, 199.17, 199.43, 197.35, 198.33},  // [4] 05-07 13:30 short signal
    {1746625500000LL, 198.33, 198.69, 197.69, 197.8},   // [5] 13:45 flip bar
    {1746626400000LL, 197.81, 199.15, 197.77, 199.05},  // [6] 14:00
    {1746627300000LL, 199.07, 199.44, 198.81, 199.22},  // [7] 14:15
    {1746628200000LL, 199.23, 199.4, 198.85, 199.01},   // [8] 14:30
    {1746629100000LL, 199, 199.05, 197.44, 197.47},     // [9] 14:45
    {1746630000000LL, 197.45, 197.5, 194.25, 194.4},    // [10] 15:00
    {1746630900000LL, 194.41, 194.96, 193.81, 194.32},  // [11] 15:15
    {1746631800000LL, 194.35, 194.66, 193.25, 194.41},  // [12] 15:30
    {1746632700000LL, 194.41, 195.12, 193.9, 195.1},    // [13] 15:45
    {1746633600000LL, 195.1, 195.27, 194.56, 195.1},    // [14] 16:00
    {1746634500000LL, 195.1, 195.37, 194.52, 194.86},   // [15] 16:15
    {1746635400000LL, 194.87, 195.55, 194.56, 195.53},  // [16] 16:30
    {1746636300000LL, 195.51, 195.71, 195.28, 195.61},  // [17] 16:45
    {1746637200000LL, 195.6, 195.76, 195.22, 195.58},   // [18] 17:00
    {1746638100000LL, 195.59, 195.64, 195.05, 195.32},  // [19] 17:15
    {1746639000000LL, 195.31, 195.35, 194.5, 194.64},   // [20] 17:30
    {1746639900000LL, 194.64, 195.11, 194.23, 194.6},   // [21] 17:45
    {1746640800000LL, 194.6, 195.13, 193.3, 193.73},    // [22] 18:00
    {1746641700000LL, 193.69, 194.31, 193.46, 194.03},  // [23] 18:15
    {1746642600000LL, 194, 195.22, 193.85, 194.86},     // [24] 18:30
    {1746643500000LL, 194.89, 195.71, 194.03, 195.44},  // [25] 18:45
    {1746644400000LL, 195.41, 195.47, 194.75, 195.1},   // [26] 19:00
    {1746645300000LL, 195.09, 195.22, 194.13, 194.32},  // [27] 19:15 last re-issue
    {1746646200000LL, 194.32, 197.47, 194.29, 196.23},  // [28] 19:30 S TP2 stop
    {1746647100000LL, 196.2, 196.75, 195.06, 196.23},   // [29] 19:45
    {1746711000000LL, 197.73, 198.13, 196.25, 196.64},  // [30] 05-08 13:30 close_all
    {1746711900000LL, 196.63, 196.85, 196.08, 196.13},  // [31] 13:45 fill
};

// NASDAQ:AAPL 15, 2025-06-24 13:45Z .. 2025-06-25 15:00Z.
static const BarRow kAaplBouga0624[] = {
    {1750772700000LL, 201.87, 201.89, 200.22, 200.36},   // [0] 06-24 13:45
    {1750773600000LL, 200.38, 201.19, 200.21, 200.74},   // [1] 14:00 short signal
    {1750774500000LL, 200.76, 201.31, 200.655, 200.98},  // [2] 14:15 short fill
    {1750775400000LL, 201, 201.4, 200.9, 201.38},        // [3] 14:30 long signal
    {1750776300000LL, 201.38, 201.58, 200.75, 200.99},   // [4] 14:45 flip bar
    {1750777200000LL, 200.98, 201.36, 200.73, 201.2},    // [5] 15:00
    {1750778100000LL, 201.19, 201.45, 201, 201.44},      // [6] 15:15
    {1750779000000LL, 201.45, 202.03, 201.21, 201.99},   // [7] 15:30
    {1750779900000LL, 201.99, 202.09, 201.69, 201.9},    // [8] 15:45
    {1750780800000LL, 201.91, 202.48, 201.81, 202.2},    // [9] 16:00
    {1750781700000LL, 202.2, 202.565, 202.08, 202.45},   // [10] 16:15
    {1750782600000LL, 202.46, 203.43, 202.43, 203.35},   // [11] 16:30 L TP1
    {1750783500000LL, 203.35, 203.39, 202.29, 202.41},   // [12] 16:45
    {1750784400000LL, 202.44, 202.53, 201.99, 202.17},   // [13] 17:00
    {1750785300000LL, 202.17, 202.52, 201.94, 201.96},   // [14] 17:15
    {1750786200000LL, 201.97, 202.28, 201.86, 202.22},   // [15] 17:30
    {1750787100000LL, 202.21, 202.33, 202.04, 202.19},   // [16] 17:45
    {1750788000000LL, 202.21, 202.26, 201.44, 201.49},   // [17] 18:00
    {1750788900000LL, 201.5, 201.69, 201.31, 201.57},    // [18] 18:15
    {1750789800000LL, 201.57, 201.83, 201.47, 201.58},   // [19] 18:30
    {1750790700000LL, 201.58, 201.62, 201.31, 201.45},   // [20] 18:45
    {1750791600000LL, 201.46, 202.015, 201.43, 201.75},  // [21] 19:00
    {1750792500000LL, 201.755, 201.9, 201.525, 201.53},  // [22] 19:15
    {1750793400000LL, 201.53, 201.595, 200.82, 200.84},  // [23] 19:30
    {1750794300000LL, 200.85, 200.94, 200.27, 200.3},    // [24] 19:45
    {1750858200000LL, 201.44, 203.65, 201.2, 202.24},    // [25] 06-25 13:30
    {1750859100000LL, 202.27, 202.78, 201.68, 201.77},   // [26] 13:45
    {1750860000000LL, 201.77, 201.8, 201.35, 201.4},     // [27] 14:00 short signal
    {1750860900000LL, 201.41, 202.45, 201.24, 202.43},   // [28] 14:15 flip bar
    {1750861800000LL, 202.425, 203.17, 202.26, 203.07},  // [29] 14:30 S TP2 stop
    {1750862700000LL, 203.07, 203.1, 202.28, 202.46},    // [30] 14:45 close_all
    {1750863600000LL, 202.45, 202.56, 202.15, 202.23},   // [31] 15:00 fill
};

// NYSE:F 15 (feed 80f404ae85ef), 2025-08-07 19:45Z .. 2025-08-11 15:00Z.
static const BarRow kFordBouga0808[] = {
    {1754595900000LL, 11.25, 11.29, 11.25, 11.29},      // [0] 08-07 19:45 short signal
    {1754659800000LL, 11.305, 11.33, 11.22, 11.22},     // [1] 08-08 13:30 short fill
    {1754660700000LL, 11.225, 11.27, 11.21, 11.27},     // [2] 13:45 long signal
    {1754661600000LL, 11.27, 11.34, 11.265, 11.33},     // [3] 14:00 flip bar
    {1754662500000LL, 11.33, 11.34, 11.305, 11.315},    // [4] 14:15
    {1754663400000LL, 11.315, 11.35, 11.3, 11.325},     // [5] 14:30
    {1754664300000LL, 11.325, 11.33, 11.305, 11.32},    // [6] 14:45
    {1754665200000LL, 11.32, 11.365, 11.3, 11.305},     // [7] 15:00
    {1754666100000LL, 11.305, 11.325, 11.28, 11.325},   // [8] 15:15
    {1754667000000LL, 11.325, 11.325, 11.28, 11.28},    // [9] 15:30
    {1754667900000LL, 11.28, 11.305, 11.275, 11.305},   // [10] 15:45
    {1754668800000LL, 11.305, 11.33, 11.305, 11.315},   // [11] 16:00
    {1754669700000LL, 11.32, 11.325, 11.28, 11.285},    // [12] 16:15
    {1754670600000LL, 11.285, 11.29, 11.27, 11.285},    // [13] 16:30
    {1754671500000LL, 11.285, 11.295, 11.28, 11.295},   // [14] 16:45
    {1754672400000LL, 11.295, 11.3, 11.285, 11.295},    // [15] 17:00
    {1754673300000LL, 11.295, 11.33, 11.295, 11.325},   // [16] 17:15
    {1754674200000LL, 11.325, 11.33, 11.315, 11.33},    // [17] 17:30
    {1754675100000LL, 11.325, 11.335, 11.325, 11.33},   // [18] 17:45
    {1754676000000LL, 11.335, 11.34, 11.33, 11.335},    // [19] 18:00
    {1754676900000LL, 11.335, 11.335, 11.325, 11.325},  // [20] 18:15
    {1754677800000LL, 11.325, 11.33, 11.32, 11.325},    // [21] 18:30
    {1754678700000LL, 11.325, 11.33, 11.31, 11.315},    // [22] 18:45
    {1754679600000LL, 11.315, 11.33, 11.31, 11.325},    // [23] 19:00
    {1754680500000LL, 11.33, 11.34, 11.325, 11.335},    // [24] 19:15
    {1754681400000LL, 11.335, 11.34, 11.32, 11.325},    // [25] 19:30
    {1754682300000LL, 11.325, 11.34, 11.32, 11.335},    // [26] 19:45
    {1754919000000LL, 11.32, 11.57, 11.31, 11.535},     // [27] 08-11 13:30 TP1+TP2, long signal
    {1754919900000LL, 11.535, 11.54, 11.35, 11.4},      // [28] 13:45 long fill
    {1754920800000LL, 11.4, 11.405, 11.29, 11.295},     // [29] 14:00
    {1754921700000LL, 11.3, 11.315, 11.275, 11.295},    // [30] 14:15
    {1754922600000LL, 11.295, 11.38, 11.06, 11.16},     // [31] 14:30 L TP2 stop
    {1754923500000LL, 11.15, 11.18, 11.14, 11.165},     // [32] 14:45 close_all
    {1754924400000LL, 11.16, 11.21, 11.12, 11.13},      // [33] 15:00 fill
};

// The tapes' broker: 1x margin both sides, margin calls on, market fills at
// the next open, integer lots, mintick 0.01, no commission. FIXED default
// sizing by default (the tapes' fixed lots); PERCENT_OF_EQUITY 100 for the
// all-in reversal shapes.
class Probe : public BacktestEngine {
public:
    explicit Probe(double capital, double default_qty = 1.0) {
        initial_capital_ = capital;
        syminfo_.pointvalue = 1.0;
        syminfo_.mintick = 0.01;
        syminfo_mintick_ = 0.01;
        qty_step_ = 1.0;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = default_qty;
        commission_type_ = CommissionType::PERCENT;
        commission_value_ = 0.0;
        margin_long_ = 100.0;
        margin_short_ = 100.0;
        pyramiding_ = 0;
        slippage_ = 0;
        process_orders_on_close_ = false;
        set_margin_call_enabled(true);
    }
    std::function<void(Probe&, int)> script;
    void on_bar(const Bar& /*bar*/) override {
        if (script) script(*this, bar_index_);
    }
    void all_in() {
        default_qty_type_ = QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100.0;
    }
    void set_default_qty(double q) { default_qty_value_ = q; }
    void entry_default(const std::string& id, bool is_long) {
        strategy_entry(id, is_long, kNaN, kNaN, kNaN, "");
    }
    void entry_market(const std::string& id, bool is_long, double qty) {
        strategy_entry(id, is_long, kNaN, kNaN, qty, "");
    }
    void exit_stop(const std::string& id, const std::string& from, double stop) {
        strategy_exit(id, from, kNaN, stop);
    }
    void exit_limit_pct(const std::string& id, const std::string& from,
                        double limit, double pct) {
        strategy_exit(id, from, limit, kNaN, kNaN, kNaN, kNaN, pct);
    }
    void exit_limit_stop(const std::string& id, const std::string& from,
                         double limit, double stop) {
        strategy_exit(id, from, limit, stop);
    }
    void close_all() { strategy_close_all(); }
    bool flat() const { return position_side_ == PositionSide::FLAT; }
    int margin_call_rows() const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_comment == "Margin call") ++n;
        }
        return n;
    }
    int rows_exiting_on(int bar) const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).exit_bar_index == bar) ++n;
        }
        return n;
    }
    int long_rows() const {
        int n = 0;
        for (int i = 0; i < trade_count(); ++i) {
            if (get_trade(i).is_long) ++n;
        }
        return n;
    }
    using BacktestEngine::position_side_;
    using BacktestEngine::position_qty_;
    using BacktestEngine::position_entry_price_;
};

void print_trades(const Probe& p) {
    for (int i = 0; i < p.trade_count(); ++i) {
        const Trade& t = p.get_trade(i);
        std::printf("      trade %d: %s entry bar %d @ %.5f qty %.4f exit bar %d @ %.5f pnl %.5f [%s|%s]\n",
                    i, t.is_long ? "long" : "short", t.entry_bar_index,
                    t.entry_price, t.qty, t.exit_bar_index, t.exit_price,
                    t.pnl, t.exit_comment.c_str(), t.exit_id.c_str());
    }
}

// exit_tag: "Margin call" rows carry it as exit_comment; a bracket fill
// carries the strategy.exit id in exit_id and an empty comment; a
// strategy.close_all fill carries neither.
void check_trade(const Probe& p, int i, bool is_long, int entry_bar,
                 double entry_price, double qty, int exit_bar,
                 double exit_price, const char* exit_tag, double pnl) {
    CHECK(i < p.trade_count());
    if (i >= p.trade_count()) return;
    const Trade& t = p.get_trade(i);
    CHECK(t.is_long == is_long);
    CHECK(t.entry_bar_index == entry_bar);
    CHECK_NEAR(t.entry_price, entry_price, 1e-9);
    CHECK_NEAR(t.qty, qty, 1e-9);
    CHECK(t.exit_bar_index == exit_bar);
    CHECK_NEAR(t.exit_price, exit_price, 1e-9);
    const std::string tag(exit_tag);
    if (tag == "Margin call") {
        CHECK(t.exit_comment == "Margin call");
    } else if (!tag.empty()) {
        CHECK(t.exit_id == tag);
    } else {
        CHECK(t.exit_comment.empty());
    }
    CHECK_NEAR(t.pnl, pnl, 5e-3);
}

// ---------------------------------------------------------------------------
// A. M1 — aapl15-mcopen-willow: fixed 5457 short from the 04-21 17:45Z signal
// (fill 18:00Z @190.52, capital 1,039,850.98 = willowsportz's exact state).
// Slices 12 @190.60 (entry bar), 36 @190.91, 156 @192.09; the 04-22 13:30Z
// open prints 196.135 -> P = 196.14: x = 103.26 -> 412 (the raw open gives
// 102.999 -> 408, the engine's row); 676 @206.00 on 04-23; close_all 4165
// @203.81. TV's six rows.
// ---------------------------------------------------------------------------
void test_willow_half_tick_open_slice_412() {
    std::printf("-- A. willow 04-22 13:30Z: open slice marked at tick(196.135) = 196.14 -> 412 --\n");
    Probe p(1039850.98, 5457.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_default("S", false);
        if (bar == 45) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kAaplWillow);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 6);
    CHECK(p.margin_call_rows() == 5);
    check_trade(p, 0, false, 1, 190.52, 12.0, 1, 190.60, "Margin call", -0.96);
    check_trade(p, 1, false, 1, 190.52, 36.0, 2, 190.91, "Margin call", -14.04);
    check_trade(p, 2, false, 1, 190.52, 156.0, 7, 192.09, "Margin call", -244.92);
    check_trade(p, 3, false, 1, 190.52, 412.0, 9, 196.14, "Margin call", -2315.44);
    check_trade(p, 4, false, 1, 190.52, 676.0, 35, 206.00, "Margin call", -10464.48);
    check_trade(p, 5, false, 1, 190.52, 4165.0, 46, 203.81, "", -55352.85);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// B. M1 — algoai 06-20 13:30Z (probe rows TV#73/74): a 3867-share short
// carried into the half-tick open 198.235 with a 'Short Exit' stop at 200.00.
// Capital 770,950 puts the on-tick mark at x = 16.07 (-> 64) and the raw mark
// at x = 15.87 (-> 60, the engine's row); the stop then closes the 3803
// survivor at its level on the same bar (the extreme 200.94 comes after it on
// the O-L-H-C path: no second slice).
// ---------------------------------------------------------------------------
void test_algoai_0620_half_tick_open_slice_64_then_stop() {
    std::printf("-- B. algoai 06-20 13:30Z: 64 @198.24 then 'Short Exit' 3803 @200.00 --\n");
    Probe p(770950.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 1) {
            e.entry_market("S", false, 3867.0);
            e.exit_stop("Short Exit", "S", 200.0);
        }
    };
    std::vector<Bar> bars = to_bars(kAaplAlgoai0620);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.margin_call_rows() == 1);
    check_trade(p, 0, false, 2, 196.29, 64.0, 3, 198.24, "Margin call", -124.80);
    check_trade(p, 1, false, 2, 196.29, 3803.0, 3, 200.00, "Short Exit", -14109.13);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// C. M2 — algoai 10-30 13:30Z: the pin tape aapl15-mcopen1-stop-algoai (fixed
// 2891 short @268.27 from the 10-29 19:15Z signal, capital 775,794.02, stop
// 273.69) plus the probe's own declined reversal: an all-in Long placed at the
// 10-29 19:45Z close (E_s 771,303.79 / 269.84 -> Q 2858; 2858 x 271.96 >
// E_s at the 10-30 open -> dropped). TV prints the same three rows with and
// without the reversal: 76 @269.20 (entry bar), 1 @271.96 (open slice), then
// 'X' 2814 @273.69 AT ITS LEVEL — decline -> dormant -> slice -> revive.
// ---------------------------------------------------------------------------
void test_algoai_1030_declined_reversal_open_slice_revives_stop() {
    std::printf("-- C. algoai 10-30 13:30Z: 1 @271.96 open slice, then 'X' 2814 @273.69 at its level --\n");
    Probe p(775794.02);
    p.all_in();
    p.script = [](Probe& e, int bar) {
        if (bar == 1) {
            e.entry_market("S", false, 2891.0);
            e.exit_stop("X", "S", 273.69);
        }
        if (bar == 3) e.entry_default("L", true);   // declined at the open
    };
    std::vector<Bar> bars = to_bars(kAaplAlgoai1030);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 3);
    CHECK(p.margin_call_rows() == 2);
    CHECK(p.long_rows() == 0);
    check_trade(p, 0, false, 2, 268.27, 76.0, 2, 269.20, "Margin call", -70.68);
    check_trade(p, 1, false, 2, 268.27, 1.0, 4, 271.96, "Margin call", -3.69);
    check_trade(p, 2, false, 2, 268.27, 2814.0, 4, 273.69, "X", -15251.88);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// D. M2 — fast-scalper 07-21 13:30Z (probe rows TV#160/161): 4889 short
// @210.71 with the tape's capital 1,056,333.80 (no slice before 07-21), stop
// re-issued at the 07-18 18:45Z crossunder to 213.08 (a frozen full-position
// qty), an all-in Long reversal placed at the 19:45Z close (E_s 1,053,816 /
// 211.23 -> Q 4988; 4988 x 212.06 > E_s -> dropped at the 07-21 open). The
// dormant stop does not fill on the O-L-H path; the high 214.86 breaches:
// 268 @214.86 'Margin call' AND the revived, marketable stop closes the 4621
// remainder @214.86 on the same bar.
// ---------------------------------------------------------------------------
void test_scalper_0721_declined_reversal_cascade_revives_stop_same_bar() {
    std::printf("-- D. fast-scalper 07-21 13:30Z: 268 @214.86 slice + 'X' 4621 @214.86 same bar --\n");
    Probe p(1056333.80);
    p.all_in();
    p.script = [](Probe& e, int bar) {
        if (bar == 1) {
            e.entry_market("S", false, 4889.0);
            e.exit_stop("X", "S", 212.83);
        }
        if (bar == 24) e.exit_stop("X", "S", 213.08);   // re-issued in position
        if (bar == 28) e.entry_default("L", true);      // declined at the open
    };
    std::vector<Bar> bars = to_bars(kAaplScalper);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 2);
    CHECK(p.margin_call_rows() == 1);
    CHECK(p.long_rows() == 0);
    CHECK(p.rows_exiting_on(29) == 2);
    check_trade(p, 0, false, 2, 210.71, 268.0, 29, 214.86, "Margin call", -1112.20);
    check_trade(p, 1, false, 2, 210.71, 4621.0, 29, 214.86, "X", -19177.15);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// E. M2 control — aapl15-mcext-stop-scalper-b: fixed 5012 short from the
// 07-17 19:30Z signal (fill 19:45Z @210.71, capital 1,056,333.80), stop
// 212.83, NO reversal: 1 @210.75 (entry bar), 20 @210.87 (07-18 open), 108
// @211.76 (16:30Z high), then the stop fills at its level 212.83 x4883 on
// 07-21 with no slice (the stop precedes the extreme on the path).
// ---------------------------------------------------------------------------
void test_scalper_b_control_stop_at_level_no_slice() {
    std::printf("-- E. scalper-b control: 1 / 20 / 108 slices, then 'X' 4883 @212.83, no 07-21 slice --\n");
    Probe p(1056333.80, 5012.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 1) {
            e.entry_default("S", false);
            e.exit_stop("X", "S", 212.83);
        }
    };
    std::vector<Bar> bars = to_bars(kAaplScalper);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 4);
    CHECK(p.margin_call_rows() == 3);
    check_trade(p, 0, false, 2, 210.71, 1.0, 2, 210.75, "Margin call", -0.04);
    check_trade(p, 1, false, 2, 210.71, 20.0, 3, 210.87, "Margin call", -3.20);
    check_trade(p, 2, false, 2, 210.71, 108.0, 15, 211.76, "Margin call", -113.40);
    check_trade(p, 3, false, 2, 210.71, 4883.0, 29, 212.83, "X", -10351.96);
    CHECK(p.flat());
}

// therealbouga's layered legs: TP1 = close - 1.5 R (qty_percent 50), TP2 =
// close - 2.5 R with the stop at high + atr (R = stop - close), the default
// leg. atr is the probe's ta.atr(14); 0.88 reproduces the pinned 19:15Z stop
// 196.10 and keeps every earlier level clear of the bars it rests on.
void bouga_short_legs(Probe& e, double close, double high) {
    const double sl = high + 0.88;
    const double r = sl - close;
    e.exit_limit_pct("S TP1", "Short", close - 1.5 * r, 50.0);
    e.exit_limit_stop("S TP2", "Short", close - 2.5 * r, sl);
}

// ---------------------------------------------------------------------------
// F. M3 — therealbouga AAPL 05-07 (TV#4-6): long 236 carried; the 13:30Z
// signal issues the 502-share Short reversal with 'S TP1' (50%) and 'S TP2'
// (default), all three re-issued on every bar the condition holds (the entry
// refused by pyramiding=0). The 13:45Z open flips (236 closed @198.33, 502
// opened). The 19:15Z re-issue sets the stop at 196.10; the 19:30Z bar fires
// it: TV closes 251 ('S TP2') and holds 251 (to the 05-14 reversal; here to
// the 05-08 13:45Z close_all fill). The engine printed 502 'S TP2' — TP1
// froze 118 against the OLD long and the 13:45Z re-issue dropped it behind
// the still-deferred 100% sibling.
// ---------------------------------------------------------------------------
void test_bouga_0507_reversal_layered_split_survives_reissue() {
    std::printf("-- F. therealbouga AAPL 05-07: flip 236L -> 502S, re-issued legs, 'S TP2' stop closes 251 --\n");
    Probe p(1000000.0, 236.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 2) e.entry_default("Long", true);
        if (bar == 4) e.set_default_qty(502.0);
        const bool signal = (bar >= 4 && bar <= 21) || bar == 27;
        if (signal) {
            const BarRow& b = kAaplBouga0507[bar];
            e.entry_default("Short", false);
            bouga_short_legs(e, b.close, b.high);
        }
        if (bar == 30) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kAaplBouga0507);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 3);
    CHECK(p.margin_call_rows() == 0);
    check_trade(p, 0, true, 3, 199.88, 236.0, 5, 198.33, "", -365.80);
    check_trade(p, 1, false, 5, 198.33, 251.0, 28, 196.10, "S TP2", 559.73);
    check_trade(p, 2, false, 5, 198.33, 251.0, 31, 196.63, "", 426.70);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// G. M3 — therealbouga AAPL 06-24 (TV#20-24): short 250 carried; the 14:30Z
// signal issues the 490-share Long reversal with 'L TP1' (limit 203.26, 50%)
// and 'L TP2' (limit 204.60, stop 199.90), NOT re-issued. The 14:45Z open
// flips (250 closed @201.38). 16:30Z: 'L TP1' closes 245 @203.26 (the engine
// printed 125 — 50% of the OLD 250 — and 364 by the sibling). The 06-25
// 14:00Z signal reverses again (490 short, 'S TP1' 199.50 / 'S TP2' 198.00 +
// stop 202.61): the remaining 245 close @201.41 at the 14:15Z open, and the
// 14:30Z bar fires the new stop for exactly 245 @202.61; close_all takes the
// last 245 @202.45.
// ---------------------------------------------------------------------------
void test_bouga_0624_reversal_layered_split_without_reissue() {
    std::printf("-- G. therealbouga AAPL 06-24: flip 250S -> 490L, 'L TP1' 245 @203.26; 06-25 flip, 'S TP2' 245 @202.61 --\n");
    Probe p(1000000.0, 250.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 1) e.entry_default("Short", false);
        if (bar == 3) {
            e.set_default_qty(490.0);
            e.entry_default("Long", true);
            e.exit_limit_pct("L TP1", "Long", 203.26, 50.0);
            e.exit_limit_stop("L TP2", "Long", 204.60, 199.90);
        }
        if (bar == 27) {
            e.entry_default("Short", false);
            e.exit_limit_pct("S TP1", "Short", 199.50, 50.0);
            e.exit_limit_stop("S TP2", "Short", 198.00, 202.61);
        }
        if (bar == 30) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kAaplBouga0624);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 5);
    CHECK(p.margin_call_rows() == 0);
    check_trade(p, 0, false, 2, 200.76, 250.0, 4, 201.38, "", -155.00);
    check_trade(p, 1, true, 4, 201.38, 245.0, 11, 203.26, "L TP1", 460.60);
    check_trade(p, 2, true, 4, 201.38, 245.0, 28, 201.41, "", 7.35);
    check_trade(p, 3, false, 28, 201.41, 245.0, 29, 202.61, "S TP2", -294.00);
    check_trade(p, 4, false, 28, 201.41, 245.0, 31, 202.45, "", -254.80);
    CHECK(p.flat());
}

// ---------------------------------------------------------------------------
// H. M3 — therealbouga F 08-08 (TV#38-42): short 4591 carried; the 08-08
// 13:45Z signal issues the 8890-share Long reversal with 'L TP1' (11.43, 50%)
// and 'L TP2' (limit 11.53, stop 10.90). The 14:00Z open flips (4591 closed
// @11.27). 08-11 13:30Z gaps through both limits: 4445 @11.43 + 4445 @11.53
// (the engine printed 2293 / 6588). The 13:30Z signal re-enters from FLAT
// (8698, 'L TP1' 12.00 / 'L TP2' 12.40 + stop 11.26): filled 13:45Z @11.54,
// the 14:30Z bar fires the stop for exactly 4349 @11.26 (the flat-armed
// split, unchanged); close_all takes the last 4349 @11.16.
// ---------------------------------------------------------------------------
void test_bouga_f_0808_reversal_layered_split_and_flat_reentry() {
    std::printf("-- H. therealbouga F 08-08: flip 4591S -> 8890L, 4445 @11.43 + 4445 @11.53; flat re-entry 4349/4349 --\n");
    Probe p(1000000.0, 4591.0);
    p.script = [](Probe& e, int bar) {
        if (bar == 0) e.entry_default("Short", false);
        if (bar == 2) {
            e.set_default_qty(8890.0);
            e.entry_default("Long", true);
            e.exit_limit_pct("L TP1", "Long", 11.43, 50.0);
            e.exit_limit_stop("L TP2", "Long", 11.53, 10.90);
        }
        if (bar == 27) {
            e.set_default_qty(8698.0);
            e.entry_default("Long", true);
            e.exit_limit_pct("L TP1", "Long", 12.00, 50.0);
            e.exit_limit_stop("L TP2", "Long", 12.40, 11.26);
        }
        if (bar == 32) e.close_all();
    };
    std::vector<Bar> bars = to_bars(kFordBouga0808);
    p.run(bars.data(), (int)bars.size());
    print_trades(p);
    CHECK(p.trade_count() == 5);
    CHECK(p.margin_call_rows() == 0);
    check_trade(p, 0, false, 1, 11.31, 4591.0, 3, 11.27, "", 183.64);
    check_trade(p, 1, true, 3, 11.27, 4445.0, 27, 11.43, "L TP1", 711.20);
    check_trade(p, 2, true, 3, 11.27, 4445.0, 27, 11.53, "L TP2", 1155.70);
    check_trade(p, 3, true, 28, 11.54, 4349.0, 31, 11.26, "L TP2", -1217.72);
    check_trade(p, 4, true, 28, 11.54, 4349.0, 33, 11.16, "", -1652.62);
    CHECK(p.flat());
}

}  // namespace

int main() {
    std::printf("--- aapl15_margin_brackets (round 7 family N) ---\n");
    test_willow_half_tick_open_slice_412();
    test_algoai_0620_half_tick_open_slice_64_then_stop();
    test_algoai_1030_declined_reversal_open_slice_revives_stop();
    test_scalper_0721_declined_reversal_cascade_revives_stop_same_bar();
    test_scalper_b_control_stop_at_level_no_slice();
    test_bouga_0507_reversal_layered_split_survives_reissue();
    test_bouga_0624_reversal_layered_split_without_reissue();
    test_bouga_f_0808_reversal_layered_split_and_flat_reentry();
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
