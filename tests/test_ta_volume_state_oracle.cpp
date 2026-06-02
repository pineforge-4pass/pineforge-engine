/*
 * test_ta_volume_state_oracle.cpp — analytic-invariant oracles for the TA
 * family that has NO TradingView reference in the corpus.
 *
 * Production-readiness probe (WS1/#5). Engine-only.
 *
 * Skeptic's objection: "your volume/oscillator indicators are only checked by
 * hand-references that share the formula with the implementation — a shared
 * misread passes both." These oracles instead assert MATHEMATICAL invariants
 * that hold regardless of formula details (a violation is unambiguously a bug):
 *   - MFI is bounded [0,100]; CMO [-100,100]; WPR [-100,0], for every bar.
 *   - strictly-rising series -> MFI==100, CMO==100; strictly-falling -> 0 / -100.
 *   - WPR==0 when close is the window high, ==-100 when it is the window low.
 *   - OBV / PVT / AccDist match an INDEPENDENT textbook forward-sum recompute
 *     (catches accumulation / sign / off-by-one bugs; this is correctness vs the
 *     definition, not TV parity).
 *
 * NOTE: RCI's d^2-vs-Pearson behavior on ties needs a TV oracle and is
 * deliberately NOT asserted here (routed to the TV-parity spec).
 */

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

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

static bool near(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

namespace {
struct OHLCV { double o, h, l, c, v; };

// Deterministic varied feed (no RNG): oscillating close, volume swings.
std::vector<OHLCV> osc_feed(int n) {
    std::vector<OHLCV> f(n);
    for (int i = 0; i < n; ++i) {
        int phase = i % 24;
        int tri = (phase < 12) ? phase : (24 - phase);
        double c = 100.0 + tri * 2.0 + (i % 5);
        f[i] = { c, c + 1.5, c - 1.5, c, 200.0 + 150.0 * (i % 7) };  // strictly positive
    }
    return f;
}
}  // namespace

// Bounds hold on every non-NaN bar.
static void test_bounds_hold() {
    std::printf("test_bounds_hold\n");
    auto f = osc_feed(240);
    ta::MFI mfi(14);
    ta::CMO cmo(14);
    ta::WPR wpr(14);
    ta::TSI tsi(25, 13);
    ta::RCI rci(14);
    for (const auto& b : f) {
        double m = mfi.compute(b.c, b.v);
        double cm = cmo.compute(b.c);
        double w = wpr.compute(b.c, b.h, b.l);
        double t = tsi.compute(b.c);
        double r = rci.compute(b.c);
        if (!is_na(m)) CHECK(m >= 0.0 - 1e-9 && m <= 100.0 + 1e-9);
        if (!is_na(cm)) CHECK(cm >= -100.0 - 1e-9 && cm <= 100.0 + 1e-9);
        if (!is_na(w)) CHECK(w >= -100.0 - 1e-9 && w <= 0.0 + 1e-9);
        if (!is_na(t)) CHECK(t >= -100.0 - 1e-9 && t <= 100.0 + 1e-9);
        if (!is_na(r)) CHECK(r >= -100.0 - 1e-9 && r <= 100.0 + 1e-9);
    }
}

// NVI changes ONLY on a volume DECREASE; PVI ONLY on a volume INCREASE. This is
// a structural invariant of the indicators independent of the exact ROC formula
// (so it needs no TV oracle): on an up/equal-volume bar NVI must be unchanged,
// on a down/equal-volume bar PVI must be unchanged.
static void test_nvi_pvi_update_rule() {
    std::printf("test_nvi_pvi_update_rule\n");
    auto f = osc_feed(200);
    ta::NVI nvi;
    ta::PVI pvi;
    double prev_nvi = 0.0, prev_pvi = 0.0, prev_vol = 0.0;
    bool have_prev = false;
    for (const auto& b : f) {
        double n = nvi.compute(b.c, b.v);
        double p = pvi.compute(b.c, b.v);
        if (have_prev) {
            if (b.v >= prev_vol) CHECK(n == prev_nvi);   // no volume decrease -> NVI frozen
            if (b.v <= prev_vol) CHECK(p == prev_pvi);   // no volume increase -> PVI frozen
        }
        prev_nvi = n; prev_pvi = p; prev_vol = b.v; have_prev = true;
    }
}

// Strictly rising -> MFI==100, CMO==100 after warmup.
static void test_rising_saturates_high() {
    std::printf("test_rising_saturates_high\n");
    ta::MFI mfi(14);
    ta::CMO cmo(14);
    double m = 0, cm = 0;
    for (int i = 0; i < 60; ++i) {
        double c = 100.0 + i;                 // strictly rising
        m = mfi.compute(c, 1000.0);
        cm = cmo.compute(c);
    }
    CHECK(near(m, 100.0));
    CHECK(near(cm, 100.0));
}

// Strictly falling -> MFI==0, CMO==-100 after warmup.
static void test_falling_saturates_low() {
    std::printf("test_falling_saturates_low\n");
    ta::MFI mfi(14);
    ta::CMO cmo(14);
    double m = 0, cm = 0;
    for (int i = 0; i < 60; ++i) {
        double c = 200.0 - i;                 // strictly falling
        m = mfi.compute(c, 1000.0);
        cm = cmo.compute(c);
    }
    CHECK(near(m, 0.0));
    CHECK(near(cm, -100.0));
}

// WPR endpoints: close at window high -> 0; at window low -> -100.
static void test_wpr_endpoints() {
    std::printf("test_wpr_endpoints\n");
    {
        ta::WPR wpr(5);
        double w = 0;
        // Flat window then a final bar whose close == high == window max.
        double highs[7] = {110,110,110,110,110,110,110};
        double lows[7]  = { 90, 90, 90, 90, 90, 90, 90};
        double cl_top[7]= { 95, 95, 95, 95, 95, 95,110};  // last close at top
        for (int i = 0; i < 7; ++i) w = wpr.compute(cl_top[i], highs[i], lows[i]);
        CHECK(near(w, 0.0));
    }
    {
        ta::WPR wpr(5);
        double w = 0;
        double highs[7] = {110,110,110,110,110,110,110};
        double lows[7]  = { 90, 90, 90, 90, 90, 90, 90};
        double cl_bot[7]= { 95, 95, 95, 95, 95, 95, 90};  // last close at bottom
        for (int i = 0; i < 7; ++i) w = wpr.compute(cl_bot[i], highs[i], lows[i]);
        CHECK(near(w, -100.0));
    }
}

// OBV / PVT / AccDist match an independent textbook forward-sum.
static void test_forward_sum_identities() {
    std::printf("test_forward_sum_identities\n");
    auto f = osc_feed(200);
    ta::OBV obv;
    ta::PVT pvt;
    ta::AccDist ad;

    double ref_obv = 0.0, ref_pvt = 0.0, ref_ad = 0.0;
    double prev_close = std::numeric_limits<double>::quiet_NaN();

    for (const auto& b : f) {
        double e_obv = obv.compute(b.c, b.v);
        double e_pvt = pvt.compute(b.c, b.v);
        double e_ad = ad.compute(b.h, b.l, b.c, b.v);

        // independent textbook recompute
        if (!std::isnan(prev_close)) {
            if (b.c > prev_close) ref_obv += b.v;
            else if (b.c < prev_close) ref_obv -= b.v;
            ref_pvt += b.v * (b.c - prev_close) / prev_close;
        }
        double mfm = (b.h > b.l) ? (((b.c - b.l) - (b.h - b.c)) / (b.h - b.l)) : 0.0;
        ref_ad += mfm * b.v;
        prev_close = b.c;

        CHECK(near(e_obv, ref_obv, 1e-6));
        CHECK(near(e_pvt, ref_pvt, 1e-6));
        CHECK(near(e_ad, ref_ad, 1e-6));
    }
}

int main() {
    test_bounds_hold();
    test_rising_saturates_high();
    test_falling_saturates_low();
    test_wpr_endpoints();
    test_forward_sum_identities();
    test_nvi_pvi_update_rule();
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
