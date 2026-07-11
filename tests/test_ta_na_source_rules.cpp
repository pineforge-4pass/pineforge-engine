/*
 * test_ta_na_source_rules.cpp — TradingView na-source semantics for
 * ta.ema / ta.sma (KI-66), pinned trade-for-trade by the
 * pf-probe-ki66-ema-sma-na-rule export (0/11,060 mismatched decision bars
 * per leg once these rules hold).
 *
 * PINNED TV RULES (probe-adjudicated 2026-07-11):
 *   ta.ema(src,N): an na input NEVER updates and NEVER resets the recursion;
 *     the FUNCTION RETURNS NA on the na-input bar. Valid bars resume the
 *     recursion over the valid inputs only — i.e. exactly the engine's
 *     existing ta.rma na-branch pattern (RMA::compute returns na, leaves
 *     state untouched).
 *   ta.sma(src,N): an na input never enters the window (the window is the
 *     last N VALID values — the engine already compacts). Once N valid
 *     values have been seen, the function RETURNS THE BUFFER MEAN on EVERY
 *     bar INCLUDING na-input bars (a HELD non-na output). Before seeding,
 *     an na input still yields na.
 *
 * RED rows (must FAIL against unmodified HEAD 8b5932f, pass after the fix):
 *   R1  warm EMA + na input  -> returns NA that bar         (HEAD returns held)
 *   R2  seeded SMA + na input -> returns held mean          (HEAD returns na)
 *   G5  32-bar na run keeps a seeded SMA output held        (HEAD returns na)
 *
 * G / characterization rows (must PASS on BOTH HEAD and post-fix — they pin
 * the behaviour the fix MUST NOT disturb):
 *   G1  pre-seed EMA + na stays na
 *   G2  pre-seed SMA + na stays na
 *   G3  RMA na behaviour unchanged (guards the scoped-out reference path)
 *   G4  EMA state-continuity across an na run (resume value == no-na control)
 *   G6  KI-55 na_warmup EMA: an na input during warmup neither counts nor
 *       shifts the seed bar (pre-seed behaviour identical either side of fix)
 *
 * NDEBUG-PROOF: no bare assert(); a returning CHECK macro increments a
 * failure counter and main() returns nonzero, so a Release (-DNDEBUG) run
 * cannot pass vacuously. All reference values are exact in IEEE-754 double.
 */

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);\
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

// Exact IEEE-754 equality with na-aware handling. Every reference value in
// this fixture is representable exactly (halves and thirds-of-60), so exact
// equality is the correct — and strictest — check.
static bool eq(double a, double b) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return a == b;
}

// --------------------------------------------------------------------
// R1 — warm EMA + na input returns NA that bar, then resumes the exact
// recursion on the next valid bar (state untouched by the na).
//
// EMA(3): alpha = 2/(3+1) = 0.5, default src-seed mode.
//   compute(10) -> seed 10
//   compute(20) -> 0.5*20 + 0.5*10 = 15
//   compute(na) -> NA           (HEAD: returns held 15  -> RED)
//   compute(30) -> 0.5*30 + 0.5*15 = 22.5   (resume; invariant across fix)
// --------------------------------------------------------------------
static void test_ema_warm_na_returns_na_and_resumes() {
    std::printf("test_ema_warm_na_returns_na_and_resumes\n");
    ta::EMA ema(3);
    CHECK(eq(ema.compute(10.0), 10.0));                 // seed
    CHECK(eq(ema.compute(20.0), 15.0));                 // recursion
    CHECK(is_na(ema.compute(na<double>())));            // R1: NA on na bar
    CHECK(eq(ema.compute(30.0), 22.5));                 // resumes over valid inputs
}

// --------------------------------------------------------------------
// R2 — seeded SMA + na input returns the held buffer mean; the na does not
// enter the window, so the next valid bar's window is the pre-na window
// shifted by that one valid sample only.
//
// SMA(3):
//   10,20 -> warmup na ; 30 -> seed mean(10,20,30) = 20
//   compute(na) -> 20   (HELD)      (HEAD: returns na -> RED)
//   compute(40) -> mean(20,30,40) = 30   (na skipped; window compacted)
// --------------------------------------------------------------------
static void test_sma_seeded_na_returns_held_mean() {
    std::printf("test_sma_seeded_na_returns_held_mean\n");
    ta::SMA sma(3);
    CHECK(is_na(sma.compute(10.0)));
    CHECK(is_na(sma.compute(20.0)));
    CHECK(eq(sma.compute(30.0), 20.0));                 // seed
    CHECK(eq(sma.compute(na<double>()), 20.0));         // R2: held mean on na bar
    CHECK(eq(sma.compute(40.0), 30.0));                 // window {20,30,40}; na skipped
}

// --------------------------------------------------------------------
// G1 — pre-seed EMA + na stays na (invariant: na return during warmup is na
// on both HEAD and post-fix because output_val is still na). The na must
// also not advance the seed.
// --------------------------------------------------------------------
static void test_ema_preseed_na_stays_na() {
    std::printf("test_ema_preseed_na_stays_na\n");
    ta::EMA ema(3);
    CHECK(is_na(ema.compute(na<double>())));            // pre-seed na -> na
    CHECK(eq(ema.compute(10.0), 10.0));                 // seeds on first valid (na was a no-op)
    CHECK(eq(ema.compute(20.0), 15.0));
}

// --------------------------------------------------------------------
// G2 — pre-seed SMA + na stays na, and does not consume a warmup slot.
// --------------------------------------------------------------------
static void test_sma_preseed_na_stays_na() {
    std::printf("test_sma_preseed_na_stays_na\n");
    ta::SMA sma(3);
    CHECK(is_na(sma.compute(na<double>())));            // pre-seed na -> na
    CHECK(is_na(sma.compute(10.0)));                    // warmup 1/3 (na not counted)
    CHECK(is_na(sma.compute(20.0)));                    // warmup 2/3
    CHECK(eq(sma.compute(30.0), 20.0));                 // seed on the 3rd VALID value
}

// --------------------------------------------------------------------
// G3 — RMA na behaviour unchanged. RMA is the pinned reference and is
// explicitly OUT OF SCOPE for the fix; this row fails loudly if RMA is
// accidentally touched. na returns na and does not advance bar_count, so the
// seed lands on the 3rd VALID value.
// --------------------------------------------------------------------
static void test_rma_na_characterization_unchanged() {
    std::printf("test_rma_na_characterization_unchanged\n");
    ta::RMA rma(3);
    CHECK(is_na(rma.compute(10.0)));
    CHECK(is_na(rma.compute(20.0)));
    CHECK(is_na(rma.compute(na<double>())));            // na -> na, bar_count stays 2
    CHECK(eq(rma.compute(30.0), 20.0));                 // seed mean(10,20,30) on 3rd valid
}

// --------------------------------------------------------------------
// G4 — EMA state continuity across an na RUN. The recursion value produced
// on the first valid bar AFTER a run of na inputs must equal the value a
// control EMA (never fed the na run) produces — i.e. na inputs are fully
// transparent to the recursion state. Invariant across the fix (the na
// branch never mutates output_val either way).
// --------------------------------------------------------------------
static void test_ema_state_continuity_across_na_run() {
    std::printf("test_ema_state_continuity_across_na_run\n");
    ta::EMA ema(3);
    ema.compute(10.0);                                  // seed 10
    ema.compute(20.0);                                  // 15
    for (int i = 0; i < 5; ++i) ema.compute(na<double>());  // 5-bar na run
    double resume = ema.compute(30.0);                  // must be 22.5

    ta::EMA ctrl(3);
    ctrl.compute(10.0);
    ctrl.compute(20.0);
    double ctrl_v = ctrl.compute(30.0);                 // no na run

    CHECK(eq(resume, 22.5));
    CHECK(eq(resume, ctrl_v));                           // na run fully transparent
}

// --------------------------------------------------------------------
// G5 — a 32-bar na run keeps a seeded SMA output HELD at the buffer mean the
// whole way through (RED on HEAD, which returns na for the run). After the
// run the next valid bar's window still excludes every na.
// --------------------------------------------------------------------
static void test_sma_32bar_na_run_holds() {
    std::printf("test_sma_32bar_na_run_holds\n");
    ta::SMA sma(3);
    CHECK(is_na(sma.compute(10.0)));
    CHECK(is_na(sma.compute(20.0)));
    CHECK(eq(sma.compute(30.0), 20.0));                 // seed
    for (int i = 0; i < 32; ++i) {
        double v = sma.compute(na<double>());
        CHECK(eq(v, 20.0));                             // held throughout the na run
    }
    CHECK(eq(sma.compute(40.0), 30.0));                 // window {20,30,40}; 32 na's skipped
}

// --------------------------------------------------------------------
// G6 — KI-55 na_warmup EMA pre-seed unaffected. Under the security
// range-start na_warmup mode, an na input DURING warmup must (a) return na
// and (b) not count toward the `length` seed accumulation, so the SMA seed
// still lands on the length-th VALID value. Identical on HEAD and post-fix.
//
// na_warmup EMA(3): seed = SMA of the first 3 VALID values.
//   compute(10) -> na (1/3)
//   compute(na) -> na  (NOT counted)
//   compute(20) -> na (2/3)
//   compute(30) -> seed mean(10,20,30) = 20
// --------------------------------------------------------------------
static void test_ema_na_warmup_preseed_unaffected() {
    std::printf("test_ema_na_warmup_preseed_unaffected\n");
    // Latch happens on the first compute(); raise the flag before construction-
    // adjacent first use, then lower it so no other instance latches warmup.
    ta::ema_na_warmup_flag() = true;
    ta::EMA ema(3);
    CHECK(is_na(ema.compute(10.0)));                    // warmup 1/3
    CHECK(is_na(ema.compute(na<double>())));            // na during warmup -> na, not counted
    CHECK(is_na(ema.compute(20.0)));                    // warmup 2/3
    CHECK(eq(ema.compute(30.0), 20.0));                 // seed on 3rd VALID value
    ta::ema_na_warmup_flag() = false;                   // restore global for later tests
}

int main() {
    test_ema_warm_na_returns_na_and_resumes();          // R1
    test_sma_seeded_na_returns_held_mean();             // R2
    test_ema_preseed_na_stays_na();                     // G1
    test_sma_preseed_na_stays_na();                     // G2
    test_rma_na_characterization_unchanged();           // G3
    test_ema_state_continuity_across_na_run();          // G4
    test_sma_32bar_na_run_holds();                      // G5 (RED on HEAD)
    test_ema_na_warmup_preseed_unaffected();            // G6

    std::printf("\ntest_ta_na_source_rules: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
