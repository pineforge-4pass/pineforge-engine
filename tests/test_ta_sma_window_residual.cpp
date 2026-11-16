// Current corrections below are measured contract integers from OUTCOME 566cb0dc,
// decoded sha256 75783d8b81c2feb137231afc57c0d03bc3c530a94ddf9d48d3a530426bac1a54.
// Canonical-test history is not confirmed identical to the September source;
// newest-capture rule applies. Unmeasured cells and historical summaries remain.
// test_ta_sma_window_residual -- ta.sma / math.sum reproduce TradingView's
// sliding-window arithmetic to the last bit, pinned on the round-7 synthetic
// tapes (2026-09-05, pineforge-workflow scratchpad r7/pins, OANDA:EURUSD 15,
// 2025-04-01..2025-07-01, bar_index 0 = the range-start bar):
//
//   sma-pulse-n3-even / -odd     ta.sma(x, 3)  on a pulse series (below)
//   sma-pulse-n20-even           ta.sma(x, 20) on the same pulse series, P = 60
//   sma-ident-n3 / -n20          five identities per bar (A..E) against
//                                user-level references
//
// x is a pure function of bar_index -- products only, so every value is the
// same double on TradingView and here -- with a 3-bar pulse (a, b, c) at the
// start of every P-bar block and zeros elsewhere:
//     ph = bar_index % P, B = bar_index / P
//     a = 0.1 * (1 + B % 9)
//     b = 0.3 * (1 + (B / 9) % 7)
//     c = (B / 63) % 2 == 0 ? 0 : 0.7 * (1 + (B / 126) % 5)
//     x = ph == 0 ? a : ph == 1 ? b : ph == 2 ? c : 0
//
// The pulse tapes encode TradingView's value in the order qty:
//     ph >= Z : 10000000 + round(s * 1e18)  (1e19 for n = 20)     -- the raw
//               residue of an ALL-ZERO window (Z = 5 for n = 3, 22 for n = 20)
//     else    : 50000000 + round((s - e) * 1e17), e = ((x + x[1]) + ...) / n
//               the user-level newest-first re-sum
// The ident tapes encode round((s - manual / n) * 1e19) where manual is the
// user-level plain running sum S := (S + x) - nz(x[n]).
//
// What the tapes pin (every one of the 14,194 pulse decisions and 31,140
// identity checks is reproduced by the arithmetic in window_sum.hpp; this
// test replays a sample of them plus every event that identified a piece of
// the rule):
//   * an all-zero window does NOT read 0: the residue of the values that left
//     persists over 34 zero bars (bars 6..39: 9e-18, i.e. S = 2^-55);
//   * the residue changes n and 2n bars after a pulse (bars 5 and 6 of
//     block 0): the ring subtracts the COMPENSATED addend y = x - c that each
//     value entered with;
//   * a zero addend still folds the compensation (bar 2: S = 0.4 - 2^-54);
//   * bar 2522 (n = 3) / 3782 (n = 20): the 18-unit / 182-unit residue is
//     dropped to exactly 1.1 -- the incoming 0.7 swallows the carried
//     compensation (c = 2^-54 = half an ulp of 0.7), so the window is
//     re-summed newest-first and c reset; bar 2762 reads
//     (0.7 + 0.3) + 0.7000000000000001 = 1.7000000000000002, not 1.7;
//   * S is divided by n (S * (1/n) misses ~3% of the decisions);
//   * math.sum(x, n) / n == ta.sma(x, n) bit-for-bit (ident signal B: 0 at
//     every one of 6,228 bars).

#include <pineforge/ta.hpp>
#include <pineforge/math.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK_TRUE(cond, tag) do {                                            \
    if (cond) { tests_passed++; }                                             \
    else { tests_failed++; std::printf("FAIL %s\n", (tag)); }                 \
} while (0)

struct Obs { int bar; long long qty; };

// 361 observations from sma-pulse-n3-even/odd-from-20250401 (bar_index, TradingView qty)
// expectation corrected within b2..45 only: nonzero residue codes -> the current
// CAP-731-PULSE codes below; all observations outside that captured window remain historical.
static const Obs kPulseN3[] = {
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {2, 50000000}, {3, 50000000}, {4, 50000000}, {5, 10000000}, {6, 10000000}, {7, 10000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {8, 10000000}, {9, 10000000}, {10, 10000000}, {11, 10000000}, {12, 10000000}, {13, 10000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {14, 10000000}, {15, 10000000}, {16, 10000000}, {17, 10000000}, {18, 10000000}, {19, 10000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {20, 10000000}, {21, 10000000}, {22, 10000000}, {23, 10000000}, {24, 10000000}, {25, 10000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {26, 10000000}, {27, 10000000}, {28, 10000000}, {29, 10000000}, {30, 10000000}, {31, 10000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {32, 10000000}, {33, 10000000}, {34, 10000000}, {35, 10000000}, {36, 10000000}, {37, 10000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {38, 10000000}, {39, 10000000}, {40, 50000000}, {41, 50000000}, {42, 50000000}, {43, 50000000},
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    {44, 50000000}, {45, 10000000}, {46, 10000000}, {47, 10000000}, {48, 10000000}, {49, 10000000},
    {50, 10000000}, {51, 10000000}, {52, 10000000}, {53, 10000000}, {54, 10000000}, {55, 10000000},
    {56, 10000000}, {57, 10000000}, {58, 10000000}, {59, 10000000}, {60, 10000000}, {61, 10000000},
    {62, 10000000}, {63, 10000000}, {64, 10000000}, {65, 10000000}, {66, 10000000}, {67, 10000000},
    {68, 10000000}, {69, 10000000}, {70, 10000000}, {71, 10000000}, {72, 10000000}, {73, 10000000},
    {74, 10000000}, {75, 10000000}, {76, 10000000}, {77, 10000000}, {78, 10000000}, {79, 10000000},
    {80, 50000000}, {81, 50000000}, {82, 50000000}, {83, 50000000}, {84, 50000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {85, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {86, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {87, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {88, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {89, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {90, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {91, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {92, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {93, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {94, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {95, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {96, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {97, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {98, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {99, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {100, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {101, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {102, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {103, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {104, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {105, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {106, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {107, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {108, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {109, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {110, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {111, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {112, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {113, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {114, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {115, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {116, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {117, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {118, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {119, 10000000},
    // expectation corrected: 50000003 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {120, 50000000},
    // expectation corrected: 50000006 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {121, 50000000},

    // expectation corrected: 50000006 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {122, 50000000},
    // expectation corrected: 50000003 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {123, 50000000},
    // expectation corrected: 50000002 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {124, 50000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {125, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {126, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {127, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {128, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {129, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {130, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {131, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {132, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {133, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {134, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {135, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {136, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {137, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {138, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {139, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {140, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {141, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {142, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {143, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {144, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {145, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {146, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {147, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {148, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {149, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {150, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {151, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {152, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {153, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {154, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {155, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {156, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {157, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {158, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {159, 10000000}, {160, 50000000}, {161, 50000000}, {162, 50000000},
    // expectation corrected: 50000003 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {163, 50000000},
    {164, 50000000}, {165, 10000000}, {166, 10000000}, {167, 10000000}, {168, 10000000}, {169, 10000000},
    {170, 10000000}, {171, 10000000}, {172, 10000000}, {173, 10000000}, {174, 10000000}, {175, 10000000},
    {176, 10000000}, {177, 10000000}, {178, 10000000}, {179, 10000000}, {180, 10000000}, {181, 10000000},
    {182, 10000000}, {183, 10000000}, {184, 10000000}, {185, 10000000}, {186, 10000000}, {187, 10000000},
    {188, 10000000}, {189, 10000000}, {190, 10000000}, {191, 10000000}, {192, 10000000}, {193, 10000000},
    {194, 10000000}, {195, 10000000}, {196, 10000000}, {197, 10000000}, {198, 10000000}, {199, 10000000},
    {200, 50000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2510, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2511, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2512, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2513, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2514, 10000000},

    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2515, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2516, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2517, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2518, 10000000},
    // expectation corrected: 10000333 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2519, 10000000},
    // expectation corrected: 50000033 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2520, 50000000},

    // expectation corrected: 50000033 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2521, 50000000},
    // expectation corrected: 50000000 -> 49999994; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2522, 49999994}, {2523, 50000000},
    // expectation corrected: 50000006 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2524, 50000000},
    // expectation corrected: 10000037 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2525, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2526, 10000000},

    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2527, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2528, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2529, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2530, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2531, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2532, 10000000},

    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2533, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2534, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2535, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2536, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2537, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2538, 10000000},

    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2539, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2540, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2541, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2542, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2543, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2544, 10000000},

    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2545, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2546, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2547, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2548, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2549, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2550, 10000000},

    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2551, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2552, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2553, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2554, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2555, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2556, 10000000},

    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2557, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2558, 10000000},
    // expectation corrected: 10000009 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2559, 10000000},
    // expectation corrected: 50000001 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2560, 50000000}, {2561, 50000000}, {2562, 50000000},
    {2563, 50000000}, {2564, 50000000}, {2565, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2566, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2567, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2568, 10000000},

    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2569, 10000000},
    // expectation corrected: 10000019 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2570, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2750, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2751, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2752, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2753, 10000000},

    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2754, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2755, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2756, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2757, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2758, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2759, 10000000},

    // expectation corrected: 49999994 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2760, 50000000}, {2761, 50000000},
    // expectation corrected: 50000000 -> 49999989; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2762, 49999989}, {2763, 50000000},
    // expectation corrected: 50000008 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2764, 50000000},
    // expectation corrected: 10000074 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2765, 10000000},

    // expectation corrected: 10000037 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2766, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2767, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2768, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2769, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2770, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2771, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2772, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2773, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2774, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2775, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2776, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2777, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2778, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2779, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2780, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2781, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2782, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2783, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2784, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2785, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2786, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2787, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2788, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2789, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2790, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2791, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2792, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2793, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2794, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2795, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2796, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2797, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2798, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2799, 10000000},
    // expectation corrected: 50000011 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2800, 50000000},
    // expectation corrected: 50000006 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2801, 50000000},
    {2802, 50000000}, {2803, 50000000}, {2804, 50000000}, {2805, 10000000}, {2806, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2807, 10000000},

    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2808, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2809, 10000000},
    // expectation corrected: 9999981 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2810, 10000000},
    // expectation corrected: 50000053 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6200, 50000000},
    // expectation corrected: 50000056 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6201, 50000000},
    // expectation corrected: 50000056 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6202, 50000000},

    // expectation corrected: 50000056 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6203, 50000000},
    // expectation corrected: 50000052 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6204, 50000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6205, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6206, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6207, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6208, 10000000},

    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6209, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6210, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6211, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6212, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6213, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6214, 10000000},

    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6215, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6216, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6217, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6218, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6219, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6220, 10000000},

    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6221, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6222, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6223, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6224, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6225, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6226, 10000000},

    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6227, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6228, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6229, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6230, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6231, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6232, 10000000},

    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6233, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6234, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6235, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6236, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6237, 10000000},
    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6238, 10000000},

    // expectation corrected: 10000518 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6239, 10000000}
};

// 231 observations from sma-pulse-n20-even-from-20250401
static const Obs kPulseN20[] = {
    {20, 50000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {22, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {24, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {26, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {28, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {30, 10000000},

    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {32, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {34, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {36, 10000000},
    // expectation corrected: 10000028 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {38, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {40, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {42, 10000000},

    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {44, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {46, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {48, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {50, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {52, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {54, 10000000},

    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {56, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {58, 10000000}, {60, 50000000}, {62, 50000000}, {64, 50000000}, {66, 50000000},
    {68, 50000000}, {70, 50000000}, {72, 50000000}, {74, 50000000}, {76, 50000000}, {78, 50000000},
    {80, 50000000}, {82, 10000000},
    // expectation corrected: 9999944 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {84, 10000000},
    // expectation corrected: 9999889 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {86, 10000000},
    // expectation corrected: 9999833 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {88, 10000000},
    // expectation corrected: 9999778 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {90, 10000000},

    // expectation corrected: 9999722 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {92, 10000000},
    // expectation corrected: 9999667 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {94, 10000000},
    // expectation corrected: 9999611 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {96, 10000000},
    // expectation corrected: 9999556 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {98, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {100, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {102, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {104, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {106, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {108, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {110, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {112, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {114, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {116, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {118, 10000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {120, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {122, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {124, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {126, 50000000},

    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {128, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {130, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {132, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {134, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {136, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {138, 50000000},

    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {140, 50000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {142, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {144, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {146, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {148, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {150, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {152, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {154, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {156, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {158, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {160, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {162, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {164, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {166, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {168, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {170, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {172, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {174, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {176, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {178, 10000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {180, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {182, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {184, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {186, 50000000},

    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {188, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {190, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {192, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {194, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {196, 50000000},
    // expectation corrected: 49999996 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {198, 50000000},

    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {200, 50000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {202, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {204, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {206, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {208, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {210, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {212, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {214, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {216, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {218, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {220, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {222, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {224, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {226, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {228, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {230, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {232, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {234, 10000000},

    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {236, 10000000},
    // expectation corrected: 9999528 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {238, 10000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {240, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {242, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {244, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {246, 50000000},

    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {248, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {250, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {252, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {254, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {256, 50000000},
    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {258, 50000000},

    // expectation corrected: 49999995 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {260, 50000000},
    // expectation corrected: 9994948 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3770, 10000000},
    // expectation corrected: 9994948 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3772, 10000000},
    // expectation corrected: 9994948 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3774, 10000000},
    // expectation corrected: 9994948 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3776, 10000000},
    // expectation corrected: 9994948 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3778, 10000000},

    // expectation corrected: 49999950 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3780, 50000000},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3782, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3784, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3786, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3788, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3790, 49999999},

    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3792, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3794, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3796, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3798, 49999999}, {3800, 50000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3802, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3804, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3806, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3808, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3810, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3812, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3814, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3816, 10000000},
    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3818, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3820, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3822, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3824, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3826, 10000000},

    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3828, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3830, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3832, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3834, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3836, 10000000},
    // expectation corrected: 10000014 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3838, 10000000},
    {3840, 50000000}, {3842, 50000000}, {3844, 50000000}, {3846, 50000000}, {3848, 50000000},
    // expectation corrected: 9999972 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4130, 10000000},

    // expectation corrected: 9999972 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4132, 10000000},
    // expectation corrected: 9999972 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4134, 10000000},
    // expectation corrected: 9999972 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4136, 10000000},
    // expectation corrected: 9999972 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4138, 10000000},
    // expectation corrected: 49999999 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4140, 50000000},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4142, 49999999},

    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4144, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4146, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4148, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4150, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4152, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4154, 49999999},

    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4156, 49999999},
    // expectation corrected: 50000000 -> 49999999; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4158, 49999999}, {4160, 50000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4162, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4164, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4166, 10000000},

    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4168, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4170, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4172, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4174, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4176, 10000000},
    // expectation corrected: 10000111 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4178, 10000000},

    // expectation corrected: 10000056 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4180, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4182, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4184, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4186, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4188, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4190, 10000000},

    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4192, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4194, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4196, 10000000},
    // expectation corrected: 10000083 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4198, 10000000},
    // expectation corrected: 50000001 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {4200, 50000000}, {4202, 50000000},
    {4204, 50000000}, {4206, 50000000}, {4208, 50000000}, {4210, 50000000}, {4212, 50000000}, {4214, 50000000},
    {4216, 50000000}, {4218, 50000000}, {4220, 50000000}, {4222, 10000000}, {4224, 10000000}, {4226, 10000000},
    {4228, 10000000},
    // expectation corrected: 50000049 -> 50000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6200, 50000000},
    // expectation corrected: 10005107 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6202, 10000000},
    // expectation corrected: 10004885 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6204, 10000000},
    // expectation corrected: 10004663 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6206, 10000000},
    // expectation corrected: 10004441 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6208, 10000000},

    // expectation corrected: 10004219 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6210, 10000000},
    // expectation corrected: 10003997 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6212, 10000000},
    // expectation corrected: 10003775 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6214, 10000000},
    // expectation corrected: 10003553 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6216, 10000000},
    // expectation corrected: 10003331 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6218, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6220, 10000000},

    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6222, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6224, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6226, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6228, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6230, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6232, 10000000},

    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6234, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6236, 10000000},
    // expectation corrected: 10003109 -> 10000000; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {6238, 10000000}
};

// 321 observations from sma-ident-n3-from-20250401: signal C = round((sma - manual/n) * 1e19), manual = the user-level plain running sum
static const Obs kIdentC3[] = {
    {4, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {6, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {8, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {10, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {12, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {14, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {16, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {18, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {20, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {22, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {24, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {26, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {28, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {30, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {32, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {34, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {36, -185},
    // derived from CAP731PULSE 19ebeefd/a149d349 + refire c67c1415;
    // C from s=0 and the unchanged plain running reference.
    {38, -185},

    // expectation corrected: -139 -> -278; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {40, -278}, {42, 0},
    // expectation corrected: 185 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {44, 0}, {46, 0}, {48, 0}, {50, 0},
    {52, 0}, {54, 0}, {56, 0}, {58, 0}, {60, 0}, {62, 0},
    {64, 0}, {66, 0}, {68, 0}, {70, 0}, {72, 0}, {74, 0},
    {76, 0}, {78, 0}, {80, 0}, {82, 0}, {84, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {86, -185},

    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {88, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {90, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {92, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {94, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {96, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {98, -185},

    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {100, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {102, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {104, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {106, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {108, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {110, -185},

    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {112, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {114, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {116, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {118, -185},
    // expectation corrected: 0 -> -278; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {120, -278},
    // expectation corrected: 0 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {122, -555},

    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {124, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {126, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {128, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {130, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {132, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {134, -185},

    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {136, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {138, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {140, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {142, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {144, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {146, -185},

    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {148, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {150, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {152, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {154, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {156, -185},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {158, -185},
    {160, 0}, {162, 0}, {164, -185}, {166, -185}, {168, -185}, {170, -185},
    {172, -185}, {174, -185}, {176, -185}, {178, -185}, {180, -185}, {182, -185},
    {184, -185}, {186, -185}, {188, -185}, {190, -185}, {192, -185}, {194, -185},
    {196, -185}, {198, -185}, {200, 0}, {202, 0},
    // expectation corrected: 0 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {204, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {206, -185},

    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {208, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {210, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {212, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {214, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {216, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {218, -185},

    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {220, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {222, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {224, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {226, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {228, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {230, -185},

    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {232, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {234, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {236, -185},
    // expectation corrected: 185 -> -185; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {238, -185},
    // expectation corrected: 0 -> -278; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {240, -278},
    // expectation corrected: 0 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {242, -1110},

    // expectation corrected: -370 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {244, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {246, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {248, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {250, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {252, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {254, -555},

    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {256, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {258, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {260, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {262, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {264, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {266, -555},

    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {268, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {270, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {272, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {274, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {276, -555},
    // expectation corrected: -185 -> -555; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {278, -555},

    // expectation corrected: -555 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {280, -1110}, {282, -555},
    // expectation corrected: -740 -> -925; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {284, -925}, {286, -925}, {288, -925}, {290, -925},
    {292, -925}, {294, -925}, {296, -925}, {298, -925}, {300, -925}, {302, -925},
    {304, -925}, {306, -925}, {308, -925}, {310, -925}, {312, -925}, {314, -925},
    {316, -925}, {318, -925}, {320, -1110}, {322, -1665},
    // expectation corrected: -1110 -> -1295; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {324, -1295}, {326, -1295},
    {328, -1295}, {330, -1295}, {332, -1295}, {334, -1295}, {336, -1295}, {338, -1295},
    {340, -1295}, {342, -1295}, {344, -1295}, {346, -1295}, {348, -1295}, {350, -1295},
    {352, -1295}, {354, -1295}, {356, -1295}, {358, -1295}, {360, -1318}, {362, -1665},
    {364, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {366, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {368, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {370, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {372, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {374, -1480},

    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {376, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {378, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {380, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {382, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {384, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {386, -1480},

    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {388, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {390, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {392, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {394, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {396, -1480},
    // expectation corrected: -1388 -> -1480; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {398, -1480},

    // expectation corrected: -1388 -> -1527; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {400, -1527},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2500, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2502, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2504, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2506, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2508, -13323},

    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2510, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2512, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2514, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2516, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2518, -13323},
    // expectation corrected: -9992 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2520, -13323},

    // expectation corrected: -13323 -> -13878; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2522, -13878},
    // expectation corrected: -12768 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2524, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2526, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2528, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2530, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2532, -13323},

    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2534, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2536, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2538, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2540, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2542, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2544, -13323},

    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2546, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2548, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2550, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2552, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2554, -13323},
    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2556, -13323},

    // expectation corrected: -13230 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2558, -13323},
    // expectation corrected: -13184 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2560, -13323}, {2562, -13323}, {2564, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2566, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2568, -13323},

    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2570, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2572, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2574, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2576, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2578, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2580, -13323},

    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2582, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2584, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2586, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2588, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2590, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2592, -13323},

    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2594, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2596, -13323},
    // expectation corrected: -13138 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2598, -13323},
    // expectation corrected: -13184 -> -13323; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2600, -13323},
    // expectation corrected: -3331 -> -6106; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3760, -6106},
    // expectation corrected: -3331 -> -6661; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3762, -6661},

    // expectation corrected: -3053 -> -6106; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3764, -6106},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3766, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3768, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3770, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3772, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3774, -5921},

    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3776, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3778, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3780, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3782, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3784, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3786, -5921},

    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3788, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3790, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3792, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3794, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3796, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3798, -5921},

    // expectation corrected: -3053 -> -5829; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3800, -5829},
    // expectation corrected: -3331 -> -5551; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3802, -5551},
    // expectation corrected: -3053 -> -6106; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3804, -6106},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3806, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3808, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3810, -5921},

    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3812, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3814, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3816, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3818, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3820, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3822, -5921},

    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3824, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3826, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3828, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3830, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3832, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3834, -5921},

    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3836, -5921},
    // expectation corrected: -2961 -> -5921; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3838, -5921},
    // expectation corrected: -3053 -> -5829; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3840, -5829},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3842, -4441},
    // expectation corrected: -555 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3844, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3846, -4441},

    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3848, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3850, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3852, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3854, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3856, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3858, -4441},

    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3860, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3862, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3864, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3866, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3868, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3870, -4441},

    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3872, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3874, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3876, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3878, -4441},
    // expectation corrected: -1110 -> -4441; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3880, -4441},
    // expectation corrected: 0 -> -3331; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3882, -3331},

    // expectation corrected: 0 -> -3886; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3884, -3886},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3886, -3701},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3888, -3701},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3890, -3701},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3892, -3701},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3894, -3701},

    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3896, -3701},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3898, -3701},
    // expectation corrected: -740 -> -3701; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3900, -3701}
};

// 313 observations from sma-ident-n20-from-20250401: signal C = round((sma - manual/n) * 1e19), manual = the user-level plain running sum
static const Obs kIdentC20[] = {

    // expectation corrected: -69 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {20, -35},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {22, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {24, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {26, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {28, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {30, -28},

    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {32, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {34, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {36, -28},
    // expectation corrected: 0 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {38, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {40, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {42, -28},

    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {44, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {46, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {48, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {50, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {52, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {54, -28},

    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {56, -28},
    // expectation corrected: -14 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {58, -28},
    // expectation corrected: -17 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {60, -35}, {62, 0}, {64, 0}, {66, 0},
    {68, 0}, {70, 0}, {72, 0}, {74, 0}, {76, 0}, {78, 0},

    // expectation corrected: 35 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {80, 0}, {82, 0},
    // expectation corrected: -56 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {84, 0},
    // expectation corrected: -111 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {86, 0},
    // expectation corrected: -167 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {88, 0},
    // expectation corrected: -222 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {90, 0},

    // expectation corrected: -278 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {92, 0},
    // expectation corrected: -333 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {94, 0},
    // expectation corrected: -389 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {96, 0},
    // expectation corrected: -444 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {98, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {100, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {102, 0},

    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {104, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {106, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {108, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {110, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {112, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {114, 0},

    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {116, 0},
    // expectation corrected: -472 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {118, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {120, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {122, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {124, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {126, 0},

    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {128, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {130, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {132, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {134, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {136, 0},
    // expectation corrected: -520 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {138, 0},

    // expectation corrected: -503 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {140, -35},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {142, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {144, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {146, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {148, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {150, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {152, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {154, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {156, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {158, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {160, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {162, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {164, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {166, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {168, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {170, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {172, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {174, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {176, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {178, -28},
    // expectation corrected: -486 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {180, -35},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {182, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {184, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {186, -69},

    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {188, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {190, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {192, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {194, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {196, -69},
    // expectation corrected: -486 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {198, -69},

    // expectation corrected: -503 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {200, -35},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {202, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {204, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {206, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {208, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {210, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {212, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {214, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {216, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {218, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {220, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {222, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {224, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {226, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {228, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {230, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {232, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {234, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {236, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {238, -28},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {240, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {242, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {244, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {246, 0},

    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {248, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {250, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {252, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {254, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {256, 0},
    // expectation corrected: -486 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {258, 0},

    // expectation corrected: -503 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {260, -35},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {262, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {264, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {266, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {268, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {270, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {272, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {274, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {276, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {278, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {280, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {282, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {284, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {286, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {288, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {290, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {292, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {294, -28},

    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {296, -28},
    // expectation corrected: -500 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {298, -28},
    // expectation corrected: -451 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {300, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {302, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {304, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {306, 0},

    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {308, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {310, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {312, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {314, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {316, 0},
    // expectation corrected: -416 -> 0; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {318, 0},

    // expectation corrected: -451 -> -35; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {320, -35},
    // expectation corrected: -416 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {322, -28},
    // expectation corrected: -361 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {324, -28},
    // expectation corrected: -305 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {326, -28},
    // expectation corrected: -250 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {328, -28},
    // expectation corrected: -194 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {330, -28},

    // expectation corrected: -139 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {332, -28},
    // expectation corrected: -83 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {334, -28}, {336, -28},
    // expectation corrected: 28 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {338, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {340, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {342, -28},

    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {344, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {346, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {348, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {350, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {352, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {354, -28},

    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {356, -28},
    // expectation corrected: 56 -> -28; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {358, -28},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {360, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {362, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {364, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {366, -69},

    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {368, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {370, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {372, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {374, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {376, -69},
    // expectation corrected: 0 -> -69; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {378, -69},

    // expectation corrected: 52 -> -87; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {380, -87},
    // expectation corrected: 28 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {382, -83},
    // expectation corrected: -28 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {384, -83}, {386, -83},
    // expectation corrected: -139 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {388, -83},
    // expectation corrected: -194 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {390, -83},

    // expectation corrected: -250 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {392, -83},
    // expectation corrected: -305 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {394, -83},
    // expectation corrected: -361 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {396, -83},
    // expectation corrected: -416 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {398, -83},
    // expectation corrected: -444 -> -83; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {400, -83},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2500, -1110},

    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2502, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2504, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2506, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2508, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2510, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2512, -1110},

    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2514, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2516, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2518, -1110},
    // expectation corrected: -7980 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2520, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2522, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2524, -1110},

    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2526, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2528, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2530, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2532, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2534, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2536, -1110},

    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2538, -1110},
    // expectation corrected: -7910 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2540, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2542, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2544, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2546, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2548, -1110},

    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2550, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2552, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2554, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2556, -1110},
    // expectation corrected: -7994 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2558, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2560, -1110},

    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2562, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2564, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2566, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2568, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2570, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2572, -1110},

    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2574, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2576, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2578, -1110},
    // expectation corrected: -8049 -> -1110; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2580, -1110},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2582, -1388},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2584, -1388},

    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2586, -1388},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2588, -1388},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2590, -1388},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2592, -1388},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2594, -1388},
    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2596, -1388},

    // expectation corrected: -8327 -> -1388; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2598, -1388},
    // expectation corrected: -8188 -> -1249; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {2600, -1249},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3760, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3762, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3764, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3766, -1998},

    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3768, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3770, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3772, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3774, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3776, -1998},
    // expectation corrected: -7050 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3778, -1998},

    // expectation corrected: -7052 -> -2004; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3780, -2004},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3782, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3784, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3786, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3788, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3790, -2082},

    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3792, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3794, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3796, -2082},
    // expectation corrected: -1943 -> -2082; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3798, -2082}, {3800, -1943},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3802, -1998},

    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3804, -1998},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3806, -1998},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3808, -1998},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3810, -1998},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3812, -1998},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3814, -1998},

    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3816, -1998},
    // expectation corrected: -1943 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3818, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3820, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3822, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3824, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3826, -1998},

    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3828, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3830, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3832, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3834, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3836, -1998},
    // expectation corrected: -1985 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3838, -1998},

    // expectation corrected: -1978 -> -1995; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3840, -1995}, {3842, -2012}, {3844, -2012}, {3846, -2012}, {3848, -2012}, {3850, -2012},
    {3852, -2012}, {3854, -2012}, {3856, -2012}, {3858, -2012}, {3860, -1943}, {3862, -1998},
    {3864, -1998}, {3866, -1998}, {3868, -1998}, {3870, -1998}, {3872, -1998}, {3874, -1998},
    {3876, -1998}, {3878, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3880, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3882, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3884, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3886, -1998},

    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3888, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3890, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3892, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3894, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3896, -1998},
    // expectation corrected: -1971 -> -1998; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3898, -1998},

    // expectation corrected: -1978 -> -1995; TV contract integer, OUTCOME 566cb0dc / decoded 75783d8b.
    // canonical-test history, not confirmed identical to the September source; newest-capture rule.
    {3900, -1995}
};

// The pulse series exactly as the pins compute it (products only).
static double pulse_x(int bar, int P) {
    const int ph = bar % P;
    const int B = bar / P;
    const double a = 0.1 * (double)(1 + B % 9);
    const double b = 0.3 * (double)(1 + (B / 9) % 7);
    const double c = ((B / 63) % 2 == 0) ? 0.0 : 0.7 * (double)(1 + (B / 126) % 5);
    return ph == 0 ? a : ph == 1 ? b : ph == 2 ? c : 0.0;
}

// Pine math.round: half away from zero.
static long long pine_round(double v) {
    const double r = std::floor(std::fabs(v) + 0.5);
    return (long long)(v < 0 ? -r : r);
}

// The pulse pins' qty encoding of an sma value at `bar`.
static long long encode_pulse(double s, const std::vector<double>& xs, int bar, int n, int zero_ph, int P) {
    if (bar % P >= zero_ph) {
        return 10000000 + pine_round(s * (n == 3 ? 1e18 : 1e19));
    }
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += xs[(std::size_t)(bar - i)];
    e = e / n;
    return 50000000 + pine_round((s - e) * 1e17);
}

static void replay_pulse(const char* tag, int n, int P, int zero_ph, const Obs* obs, std::size_t count) {
    const int last = obs[count - 1].bar;
    std::vector<double> xs((std::size_t)last + 1);
    for (int t = 0; t <= last; ++t) xs[(std::size_t)t] = pulse_x(t, P);

    ta::SMA sma(n);
    math::Sum sum(n);
    std::size_t next = 0;
    int mismatches = 0, checked = 0;
    for (int t = 0; t <= last; ++t) {
        const double s = sma.compute(xs[(std::size_t)t]);
        const double ms = sum.compute(xs[(std::size_t)t]);
        // math.sum(x, n) / n == ta.sma(x, n) bit-for-bit (ident signal B).
        if (t >= n - 1) {
            CHECK_TRUE(ms / n == s, "math.sum / n == ta.sma bit-for-bit");
        }
        if (next < count && obs[next].bar == t) {
            const long long got = encode_pulse(s, xs, t, n, zero_ph, P);
            checked++;
            if (got != obs[next].qty) {
                mismatches++;
                if (mismatches <= 5) {
                    std::printf("  %s bar %d: got qty %lld, TradingView %lld (s = %.17g)\n",
                                tag, t, got, obs[next].qty, s);
                }
            }
            next++;
        }
    }
    std::printf("  %s: %d observations replayed, %d mismatches\n", tag, checked, mismatches);
    CHECK_TRUE(mismatches == 0, tag);
}

static void replay_ident_c(const char* tag, int n, int P, const Obs* obs, std::size_t count) {
    const int last = obs[count - 1].bar;
    ta::SMA sma(n);
    double manual = 0.0;
    std::vector<double> xs((std::size_t)last + 1);
    std::size_t next = 0;
    int mismatches = 0, checked = 0;
    for (int t = 0; t <= last; ++t) {
        xs[(std::size_t)t] = pulse_x(t, P);
        const double x = xs[(std::size_t)t];
        const double s = sma.compute(x);
        // user-level `S := S + x - nz(x[n])`
        manual = (manual + x) - (t >= n ? xs[(std::size_t)(t - n)] : 0.0);
        if (next < count && obs[next].bar == t) {
            const long long got = pine_round((s - manual / n) * 1e19);
            checked++;
            if (got != obs[next].qty) {
                mismatches++;
                if (mismatches <= 5) {
                    std::printf("  %s bar %d: got %lld, TradingView %lld\n", tag, t, got, obs[next].qty);
                }
            }
            next++;
        }
    }
    std::printf("  %s: %d observations replayed, %d mismatches\n", tag, checked, mismatches);
    CHECK_TRUE(mismatches == 0, tag);
}

// The events that identified the rule, spelled out.
static void test_named_events() {
    // n = 3, block 0: pulse (0.1, 0.3, 0), then zeros.
    ta::SMA sma(3);
    std::vector<double> v;
    for (int t = 0; t < 45; ++t) v.push_back(sma.compute(pulse_x(t, 40)));
    // expectation corrected: 0.09999999999999998 -> 0.3 / 3; current TV code is 50000000.
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    CHECK_TRUE(v[3] == 0.3 / 3, "bar 3: current TV equals the newest-first reference");
    // The current capture contradicts the September residue at bars 5..39.
    CHECK_TRUE(v[4] == 0.0, "bar 4: exact 0");
    // expectation corrected: 2^-54 -> 0; current TV b5 code is 10000000.
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    CHECK_TRUE(v[5] * 3 == 0.0, "bar 5: current TV has no carried pulse residue");
    for (int t = 6; t < 40; ++t) {
        // expectation corrected: 2^-55 -> 0; current TV b6..39 codes are 10000000.
        // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
        CHECK_TRUE(v[(std::size_t)t] * 3 == 0.0, "bars 6..39: current TV has no carried pulse residue");
    }
    // Bar 40 starts the next captured pulse with no preceding zero-window residue.
    // expectation corrected: 0.20000000000000004 / 3 -> 0.2 / 3; current TV b40 code is 50000000.
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    CHECK_TRUE(v[40] == 0.2 / 3, "bar 40: current TV equals the newest-first reference");
    // Block 1 (0.2, 0.3): bar 44 is also a captured zero-window cell.
    // expectation corrected: 2^-54 -> 0; current TV b44 code is 50000000.
    // CAP-731-PULSE tv_trades.csv sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28.
    CHECK_TRUE(v[44] * 3 == 0.0, "bar 44: current TV zero window");

    // The block-63 re-sum (n = 3): S carries +18 * 2^-54 into the pulse (0.1, 0.3, 0.7);
    // the 0.7 swallows c = 2^-54, so the window is re-summed: exactly 1.1 / 3.
    ta::SMA s3(3);
    double last = na<double>();
    for (int t = 0; t <= 2522; ++t) last = s3.compute(pulse_x(t, 40));
    CHECK_TRUE(last == 1.1 / 3, "bar 2522: re-sum drops the 18-unit residue -> 1.1 / 3");
    // Without the re-sum the running sum would read 1.1 + 4 ulp.
    CHECK_TRUE(last != 1.100000000000001 / 3, "bar 2522: not the running-sum value");
    // bar 2762: the re-sum is NEWEST-FIRST: (0.7 + 0.3) + 0.7000000000000001.
    for (int t = 2523; t <= 2762; ++t) last = s3.compute(pulse_x(t, 40));
    CHECK_TRUE(last == 1.7000000000000002 / 3, "bar 2762: newest-first re-sum (0.7 + 0.3) + 0.7000000000000001");

    // n = 20: the same rule, the same events (bar 3782 = block 63 ph 2).
    ta::SMA s20(20);
    for (int t = 0; t <= 3782; ++t) last = s20.compute(pulse_x(t, 60));
    CHECK_TRUE(last == 1.1 / 20, "bar 3782 (n = 20): re-sum drops the 182-unit residue -> 1.1 / 20");
}

// recompute() replays the bar bit-for-bit, including across the re-sum event.
static void test_recompute_matches_fresh() {
    const int bars[] = {5, 6, 41, 2521, 2522, 2523, 2762, 3000};
    for (int target : bars) {
        ta::SMA a(3), b(3);
        double va = na<double>(), vb = na<double>();
        for (int t = 0; t < target; ++t) { a.compute(pulse_x(t, 40)); b.compute(pulse_x(t, 40)); }
        a.compute(12345.678);                      // a provisional value ...
        va = a.recompute(pulse_x(target, 40));     // ... replaced intrabar
        vb = b.compute(pulse_x(target, 40));
        CHECK_TRUE(va == vb, "SMA recompute == fresh compute (pulse series)");
        // and the state after the replay continues identically
        for (int t = target + 1; t < target + 50; ++t) {
            CHECK_TRUE(a.compute(pulse_x(t, 40)) == b.compute(pulse_x(t, 40)), "state after recompute continues identically");
        }
    }
}

int main() {
    replay_pulse("sma-pulse-n3 (even+odd)", 3, 40, 5, kPulseN3, sizeof(kPulseN3) / sizeof(kPulseN3[0]));
    replay_pulse("sma-pulse-n20", 20, 60, 22, kPulseN20, sizeof(kPulseN20) / sizeof(kPulseN20[0]));
    replay_ident_c("sma-ident-n3 signal C", 3, 40, kIdentC3, sizeof(kIdentC3) / sizeof(kIdentC3[0]));
    replay_ident_c("sma-ident-n20 signal C", 20, 60, kIdentC20, sizeof(kIdentC20) / sizeof(kIdentC20[0]));
    test_named_events();
    test_recompute_matches_fresh();
    std::printf("test_ta_sma_window_residual: passed=%d failed=%d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
