/*
 * test_ta_voltrend_edge.cpp — edge-path coverage for ta_volatility_trend.cpp.
 *
 * Targets the na-guards and state transitions that the existing TA tests
 * (test_ta, test_ta_indicators_extras, test_dmi_parity) skip:
 *
 *   - Supertrend: direction flip from the bearish init (+1) up to an
 *     uptrend (-1) and then back down to bearish (+1) when the close
 *     pierces the trailing band; the final_upper/final_lower min/max vs
 *     basic-band branches; the ATR warmup, replayed against TradingView's
 *     tape (tests/fixtures/supertrend_warmup).
 *   - SAR: na-input guard, the prev_close-na priming bar, the
 *     first-trend-bar init for BOTH long and short, the ep/af
 *     acceleration steps (long: high>ep, short: low<ep), and the long
 *     trend reversal when the low pierces the projected SAR.
 *   - ATR / Variance / TR: TR/ATR warmup na returns then finite values,
 *     the ATR::recompute warmup path, and Variance's na-guard +
 *     buffer-pop window maintenance.
 *
 * Every expected value below was derived by hand-tracing the Pine
 * reference recipe and confirmed bit-for-bit against the engine; a
 * refactor that breaks one of these state transitions will trip a CHECK
 * here rather than disappear into corpus drift.
 *
 * NDEBUG-PROOF: CHECK is a self-rolled failure counter (NOT assert), and
 * main() returns nonzero when any check fails, so the gate cannot pass
 * vacuously under -DNDEBUG.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

#include "exit_comment_tape.hpp"

#ifndef PINEFORGE_SUPERTREND_WARMUP_FIXTURE_DIR
#error "PINEFORGE_SUPERTREND_WARMUP_FIXTURE_DIR must name tests/fixtures/supertrend_warmup"
#endif

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                        \
        if (!(expr)) {                                                          \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                     \
        } else {                                                                \
            ++tests_passed;                                                     \
        }                                                                       \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) <= tol;
}

// ============================================================================
// Supertrend: ATR-warmup na, bearish init, flip up to -1, flip back to +1.
// ATR period 2 → ATR(bar1)=na, finite from bar2. factor 0.5 is small enough
// that the trailing band sits close to price, so the direction can flip both
// ways across a short rising-then-crashing series.
// ============================================================================

static void test_supertrend_flip() {
    std::printf("test_supertrend_flip\n");
    ta::Supertrend st(0.5, 2);

    // bar1: ATR not yet warmed up. TradingView's reference ratchets each
    // band against nz(band[1]) = 0 and, with no close[1], keeps the 0, so
    // the line is 0; the direction is 1 while atr[1] is na (tape
    // w11-st-warmup-eth15, bar 0 of every pair).
    auto r1 = st.compute(100.0, 99.0, 99.5);
    CHECK(near(r1.value, 0.0));
    CHECK(near(r1.direction, 1.0));

    // bar2: ATR warms up (= (1.0 + 1.5)/2 = 1.25). hl2 = 100.5,
    // final_upper = 100.5 + 0.5*1.25 = 101.125, final_lower = 99.875.
    // atr[1] is still na → dir = +1 (bearish). st_val = dir==1 ?
    // final_upper : final_lower = 101.125.
    auto r2 = st.compute(101.0, 100.0, 101.0);
    CHECK(near(r2.direction, 1.0));
    CHECK(near(r2.value, 101.125));

    // bar3: prev_dir == +1 and close (102) > final_upper → flip to UPTREND
    // (-1). In an uptrend the line trails the LOWER band (st_val = lower).
    auto r3 = st.compute(102.0, 101.0, 102.0);
    CHECK(near(r3.direction, -1.0));
    CHECK(near(r3.value, 100.9375));   // final_lower carried up via the max-branch

    // bar4: still uptrend (close stays above the lower band) → dir stays -1,
    // line keeps trailing the (rising) lower band.
    auto r4 = st.compute(103.0, 102.0, 103.0);
    CHECK(near(r4.direction, -1.0));
    CHECK(near(r4.value, 101.96875));

    // bar5: crash. prev_dir == -1 and close (90.5) < final_lower → flip to
    // DOWNTREND (+1) [line 183-184]; line now trails the UPPER band.
    auto r5 = st.compute(100.0, 90.0, 90.5);
    CHECK(near(r5.direction, 1.0));
    CHECK(near(r5.value, 98.515625));

    // bar6: stays bearish (close below upper band) → dir stays +1.
    auto r6 = st.compute(91.0, 89.0, 90.0);
    CHECK(near(r6.direction, 1.0));
    CHECK(near(r6.value, 92.2578125));

    // na input on any OHLC component → both fields na (lines 136-138).
    ta::Supertrend st_na(0.5, 2);
    auto rn = st_na.compute(na<double>(), 1.0, 1.0);
    CHECK(is_na(rn.value));
    CHECK(is_na(rn.direction));
}

// ============================================================================
// SAR long: prev_close-na priming bar, first-trend-bar long init, the
// high>ep ep/af acceleration steps, and the low<sar long→short reversal.
// (start 0.02, increment 0.02, maximum 0.2)
// ============================================================================

static void test_sar_long_then_flip() {
    std::printf("test_sar_long_then_flip\n");
    ta::SAR sar(0.02, 0.02, 0.2);

    // na input → na (line 327-329), no state mutation.
    CHECK(is_na(sar.compute(na<double>(), na<double>(), na<double>())));

    // bar1: prev_close still na → priming bar returns na (line 331-336).
    CHECK(is_na(sar.compute(11.0, 9.0, 10.0)));

    // bar2: init long (close 12 > prev_close 10). ep=high=13, sar=prev_low=9,
    // af=0.02. new_sar = 9 + 0.02*(13-9) = 9.08, low(10) does not pierce it.
    // first-trend-bar → no ep/af step. Clamp to prev_low(9) → 9.0.
    CHECK(near(sar.compute(13.0, 10.0, 12.0), 9.0));

    // bar3: not first bar. new_sar = 9 + 0.02*(13-9) = 9.08; low(12) safe.
    // high(15) > ep(13) → ep=15, af=0.04 (acceleration step, lines 370-373).
    // Clamp to min(prev_low=10, prev_prev_low=9) → 9.0.
    CHECK(near(sar.compute(15.0, 12.0, 14.0), 9.0));

    // bar4: new_sar = 9 + 0.04*(15-9) = 9.24; low(14) safe. high(17) > ep(15)
    // → ep=17, af=0.06. Clamp min(prev_low=12, prev_prev_low=10) leaves 9.24.
    CHECK(near(sar.compute(17.0, 14.0, 16.0), 9.24));

    // bar5: new_sar = 9.24 + 0.06*(17-9.24) = 9.7056; low(8) < new_sar →
    // long→short reversal (lines 350-357): new_sar = max(high=12, ep=17) = 17,
    // af reset to 0.02, ep = low = 8. Clamp short to recent highs → 17.0.
    CHECK(near(sar.compute(12.0, 8.0, 9.0), 17.0));
}

// ============================================================================
// SAR short: first-trend-bar SHORT init (line 341-343) and the low<ep ep/af
// acceleration step for a short position (lines 376-377).
// ============================================================================

static void test_sar_short_init() {
    std::printf("test_sar_short_init\n");
    ta::SAR sar(0.02, 0.02, 0.2);

    // bar1: priming → na.
    CHECK(is_na(sar.compute(20.0, 18.0, 19.0)));

    // bar2: init short (close 17 < prev_close 19). ep=low=16, sar=prev_high=20,
    // af=0.02. new_sar = 20 + 0.02*(16-20) = 19.92; high(19) does not pierce.
    // Clamp short to prev_high(20) → 20.0.
    CHECK(near(sar.compute(19.0, 16.0, 17.0), 20.0));

    // bar3: not first bar. new_sar = 20 + 0.02*(16-20) = 19.92; high(17) safe.
    // low(14) < ep(16) → ep=14, af=0.04 (short ep/af step, lines 376-377).
    // Clamp short to max(prev_high=19, prev_prev_high=20) → 20.0.
    CHECK(near(sar.compute(17.0, 14.0, 15.0), 20.0));

    // bar4: new_sar = 20 + 0.04*(14-20) = 19.76; high(15) safe. low(12) < ep(14)
    // → ep=12, af=0.06. Clamp short to max(prev_high=17, prev_prev_high=19)
    // = 19; 19.76 already >= 19 so the max keeps 19.76.
    CHECK(near(sar.compute(15.0, 12.0, 13.0), 19.76));
}

// ============================================================================
// ATR / TR warmup + ATR::recompute warmup, and Variance na-guard + buffer pop.
// ============================================================================

static void test_atr_tr_warmup() {
    std::printf("test_atr_tr_warmup\n");
    const double h[] = {10, 12, 14, 15};
    const double l[] = {8, 9, 10, 12};
    const double c[] = {9, 11, 13, 14};

    // ATR(3): RMA warmup → na for first length-1 bars, finite from bar3.
    // TRs: bar1 = high-low = 2; bar2 = max(3,|12-9|,|9-9|)=3; bar3 =
    // max(4,|14-11|,|10-11|)=4 → RMA(3) at bar3 = (2+3+4)/3 = 3.0.
    ta::ATR atr(3);
    CHECK(is_na(atr.compute(h[0], l[0], c[0])));
    CHECK(is_na(atr.compute(h[1], l[1], c[1])));
    CHECK(near(atr.compute(h[2], l[2], c[2]), 3.0));
    // bar4: TR = max(3,|15-13|,|12-13|)=3 → RMA = (3 + 2*3)/3 = 3.0.
    CHECK(near(atr.compute(h[3], l[3], c[3]), 3.0));

    // ATR::recompute warmup path (lines 587-608): advance to a finite bar,
    // then recompute the same bar — must match a fresh compute of the same
    // OHLC. Also exercises ATR::recompute na-input and warmup-na returns.
    {
        ta::ATR a(3), b(3);
        for (int i = 0; i < 3; ++i) { a.compute(h[i], l[i], c[i]); b.compute(h[i], l[i], c[i]); }
        a.compute(h[3], l[3], c[3]);
        double ar = a.recompute(16.0, 11.0, 15.0);
        double br = b.compute(16.0, 11.0, 15.0);
        CHECK(!is_na(ar));
        CHECK(near(ar, br));

        // recompute na input → na.
        ta::ATR cna(3);
        cna.compute(10.0, 8.0, 9.0);
        CHECK(is_na(cna.recompute(na<double>(), na<double>(), na<double>())));

        // recompute while still in warmup (only 2 bars total) → na.
        ta::ATR cwarm(3);
        cwarm.compute(10.0, 8.0, 9.0);
        CHECK(is_na(cwarm.recompute(11.0, 9.0, 10.0)));
    }

    // TR (handle_na=false): first bar na, then finite. (handle_na=true → high-low)
    ta::TR tr_false(false);
    CHECK(is_na(tr_false.compute(h[0], l[0], c[0])));
    CHECK(near(tr_false.compute(h[1], l[1], c[1]), 3.0));
    CHECK(near(tr_false.compute(h[2], l[2], c[2]), 4.0));

    ta::TR tr_true(true);
    CHECK(near(tr_true.compute(h[0], l[0], c[0]), 2.0));   // high - low on first bar
    CHECK(near(tr_true.compute(h[1], l[1], c[1]), 3.0));

    // Variance(3): na-guard + warmup na + buffer pop maintaining a size-3
    // window. Feeding {2,4,6,8,10}; window {2,4,6} → biased var = 8/3.
    ta::Variance var(3);
    CHECK(is_na(var.compute(2.0)));
    CHECK(is_na(var.compute(4.0)));
    CHECK(near(var.compute(6.0), 8.0 / 3.0, 1e-12));   // mean 4, ((-2)^2+0+2^2)/3
    // pop oldest (2): window {4,6,8} → mean 6 → 8/3 again.
    CHECK(near(var.compute(8.0), 8.0 / 3.0, 1e-12));
    // pop oldest (4): window {6,8,10} → still 8/3.
    CHECK(near(var.compute(10.0), 8.0 / 3.0, 1e-12));
    // na input → na (line 458-460), window untouched.
    CHECK(is_na(var.compute(na<double>())));
}

// ============================================================================
// MACD: EMAs seed on the first non-na value (Pine ta.ema), so MACD is finite
// from bar 1 — exercises the fast/slow EMA path and the histogram = macd-signal
// branch in MACD::compute, plus the recompute mirror.
// ============================================================================

static void test_macd_seeded() {
    std::printf("test_macd_seeded\n");
    ta::MACD macd(3, 5, 2);
    // Bar 1: both EMAs seed to src=10 → macd_line = 0; signal EMA seeds to
    // macd_line=0 → signal_line = 0, histogram = 0.
    auto r0 = macd.compute(10.0);
    CHECK(near(r0.macd_line, 0.0));
    CHECK(near(r0.signal_line, 0.0));
    CHECK(near(r0.histogram, 0.0));

    // After a rising series macd_line > 0 (fast EMA leads slow EMA up) and
    // histogram = macd_line - signal_line is finite.
    ta::MACDResult last;
    for (int i = 1; i <= 20; ++i) last = macd.compute(10.0 + i);
    CHECK(std::isfinite(last.macd_line));
    CHECK(last.macd_line > 0.0);
    CHECK(std::isfinite(last.histogram));
    CHECK(near(last.histogram, last.macd_line - last.signal_line, 1e-9));

    // recompute on the same bar reproduces the last compute exactly.
    auto rr = macd.recompute(30.0);
    CHECK(near(rr.macd_line, last.macd_line));
    CHECK(near(rr.signal_line, last.signal_line));
    CHECK(near(rr.histogram, last.histogram));
}

// ============================================================================
// A recalculation of a bar (calc_on_order_fills, a realtime tick: generated
// code calls recompute on a bar it has computed) answers what one computation
// of that bar from the previous bar's state answers, and leaves the state a
// computation leaves: the ATR inside advances once per bar. It advanced twice
// (recompute restored the bands but computed the ATR again), so every later
// bar of such a run read a wrong line.
// ============================================================================

static void test_supertrend_recompute_is_one_computation() {
    std::printf("test_supertrend_recompute_is_one_computation\n");
    const double bars[][3] = {{101.0, 99.0, 100.0},  {103.0, 100.0, 102.0},
                              {104.0, 101.0, 101.5}, {102.0, 97.0, 98.0},
                              {99.0, 95.0, 96.0},    {100.0, 96.0, 99.5}};
    ta::Supertrend once(3.0, 2);
    ta::Supertrend again(3.0, 2);
    for (const auto& bar : bars) {
        const auto want = once.compute(bar[0], bar[1], bar[2]);
        // A first tick of the bar, then the bar's recalculation at its close.
        again.compute(bar[0] + 0.5, bar[1] + 0.25, bar[2] + 0.125);
        const auto got = again.recompute(bar[0], bar[1], bar[2]);
        CHECK(near(got.value, want.value));
        CHECK(near(got.direction, want.direction));
    }
    // An na recalculation leaves no advance of its own behind either.
    ta::Supertrend na_tick(3.0, 2);
    ta::Supertrend plain(3.0, 2);
    for (const auto& bar : bars) {
        const auto want = plain.compute(bar[0], bar[1], bar[2]);
        na_tick.compute(bar[0], bar[1], bar[2]);
        const auto blank = na_tick.recompute(na<double>(), na<double>(), na<double>());
        CHECK(is_na(blank.value) && is_na(blank.direction));
        const auto got = na_tick.recompute(bar[0], bar[1], bar[2]);
        CHECK(near(got.value, want.value));
        CHECK(near(got.direction, want.direction));
    }
}

// ============================================================================
// Supertrend from the run's first bar, as TradingView computes it: the Pine
// reference (hl2 +/- factor * ta.atr(atrPeriod), each band ratcheted against
// nz(band[1]) and close[1], direction 1 while atr[1] is na). Replayed against
// w11-st-warmup-eth15 (lab tv --no-note, BINANCE:ETHUSDT.P 15, 2025-04-01 ..
// 04-03): each exit comment spells "line,direction" of ta.supertrend(3, 10),
// (2, 1), (1000, 5), (0.5, 3) and (input 3, input 7) on the bar before the
// fill, the line to two decimals; " @..." carries bar 0's. The bars are the
// corpus feed's own (fixtures/supertrend_warmup/bars.inc).
// ============================================================================

struct StBar {
    int64_t ts;
    double o, h, l, c;
};
#include "fixtures/supertrend_warmup/bars.inc"

// TradingView's "#.##" spelling against the engine's value: "n" for na, else
// the number the engine's line rounds to.
static bool same_spelled(const std::string& text, double value) {
    if (text == "n") return is_na(value);
    if (is_na(value)) return false;
    char* end = nullptr;
    const double tv = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    return std::fabs(value - tv) <= 0.005 + 1e-9;
}

static void test_supertrend_warmup_tape() {
    std::printf("test_supertrend_warmup_tape\n");
    constexpr int kSites = 5;
    const double factor[kSites] = {3.0, 2.0, 1000.0, 0.5, 3.0};
    const int period[kSites] = {10, 1, 5, 3, 7};
    constexpr std::size_t kBars = sizeof(kEth15) / sizeof(kEth15[0]);
    std::vector<std::string> engine(kBars);
    {
        std::vector<ta::Supertrend> sites;
        for (int k = 0; k < kSites; ++k) sites.emplace_back(factor[k], period[k]);
        for (std::size_t i = 0; i < kBars; ++i) {
            std::string spelled;
            for (int k = 0; k < kSites; ++k) {
                const auto r = sites[static_cast<std::size_t>(k)].compute(kEth15[i].h, kEth15[i].l,
                                                                          kEth15[i].c);
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.17g,%.17g", r.value, r.direction);
                spelled += (k ? "|" : "") + std::string(buf);
            }
            engine[i] = spelled;
        }
    }
    // One reading against the engine's row: every site's line and direction.
    const auto agrees = [&](const std::string& tv, std::size_t row) {
        const auto sites_tv = exit_comment_tape::split(tv, '|');
        const auto sites_engine = exit_comment_tape::split(engine[row], '|');
        if (sites_tv.size() != kSites || sites_engine.size() != kSites) return false;
        for (int k = 0; k < kSites; ++k) {
            const auto a = exit_comment_tape::split(sites_tv[static_cast<std::size_t>(k)], ',');
            const auto b = exit_comment_tape::split(sites_engine[static_cast<std::size_t>(k)], ',');
            if (a.size() != 2 || b.size() != 2) return false;
            const double line = std::strtod(b[0].c_str(), nullptr);
            const double direction = std::strtod(b[1].c_str(), nullptr);
            if (!same_spelled(a[0], line) || !same_spelled(a[1], direction)) return false;
        }
        return true;
    };
    bool ok = true;
    const auto readings = exit_comment_tape::read(PINEFORGE_SUPERTREND_WARMUP_FIXTURE_DIR,
                                                  "w11-st-warmup-eth15", ok);
    CHECK(ok);
    int compared = 0, wrong = 0, bar0 = 0;
    for (const auto& reading : readings) {
        const std::size_t at = reading.signal.find(" @");
        if (at == std::string::npos) { ++wrong; continue; }
        const std::int64_t row64 = (reading.bar_ms - kEth15[0].ts) / (15 * 60000);
        if (row64 < 0 || row64 >= static_cast<std::int64_t>(kBars)
            || kEth15[static_cast<std::size_t>(row64)].ts != reading.bar_ms) {
            ++wrong;
            continue;
        }
        ++compared;
        if (!agrees(reading.signal.substr(0, at), static_cast<std::size_t>(row64))) {
            if (++wrong <= 5) {
                std::printf("  bar %lld: tv %s engine %s\n",
                            static_cast<long long>(reading.bar_ms),
                            reading.signal.substr(0, at).c_str(),
                            engine[static_cast<std::size_t>(row64)].c_str());
            }
        }
        bar0 += agrees(reading.signal.substr(at + 2), 0) ? 1 : 0;
    }
    std::printf("  w11-st-warmup-eth15: %d bars compared, %d differ; bar 0 agrees on %d\n",
                compared, wrong, bar0);
    CHECK(compared == static_cast<int>(kBars) - 1);
    CHECK(wrong == 0);
    CHECK(bar0 == compared);
}

int main() {
    test_supertrend_flip();
    test_supertrend_warmup_tape();
    test_supertrend_recompute_is_one_computation();
    test_sar_long_then_flip();
    test_sar_short_init();
    test_atr_tr_warmup();
    test_macd_seeded();

    std::printf("\nta_voltrend_edge: %d passed, %d failed\n",
                tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
