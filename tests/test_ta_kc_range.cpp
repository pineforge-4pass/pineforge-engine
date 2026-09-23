/*
 * test_ta_kc_range.cpp — ta::KC / ta::KCW take useTrueRange (R5 lane TA1).
 *
 * TradingView's Pine v6 reference for ta.kc and ta.kcw(series, length, mult,
 * useTrueRange): useTrueRange "An optional parameter. Specifies if True Range
 * is used; default is true. If the value is false, the range will be
 * calculated with the expression (high - low)." Spelled out:
 *
 *     f_kc(src, length, mult, useTrueRange) =>
 *         float basis = ta.ema(src, length)
 *         float span = (useTrueRange) ? ta.tr : (high - low)
 *         float rangeEma = ta.ema(span, length)
 *         [basis, basis + rangeEma * mult, basis - rangeEma * mult]
 *
 *     f_kcw(...) => ((basis + rangeEma * mult) - (basis - rangeEma * mult)) / basis
 *
 * Witness: KC(length, mult, use_true_range) and KCW(...) against that
 * recomputation -- ta::EMA for both EMAs, ta.tr(false) or high - low for the
 * span -- bit for bit on every bar, for both values of the flag, over na
 * sources and na high / low / close, and through compute -> recompute ->
 * recompute sequences. Omitting the flag is useTrueRange = true, value for
 * value.
 */

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr, ...)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL %s:%d %s -- ", __FILE__, __LINE__, #expr);     \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return std::memcmp(&a, &b, sizeof a) == 0;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

struct Bars { std::vector<double> src, high, low, close; };

static Bars make_bars(int n, uint64_t seed, bool with_na) {
    Rng r(seed);
    Bars b;
    double c = 3200.0;
    for (int i = 0; i < n; ++i) {
        const double o = c;
        c = o + (r.unit() - 0.5) * 25.0;   // gaps: the open is the last close,
        const double h = std::max(o, c) + r.unit() * 8.0;   // the range may not
        const double l = std::min(o, c) - r.unit() * 8.0;   // reach it
        double s = c, hh = h + (r.unit() < 0.2 ? 15.0 : 0.0), ll = l, cc = c;
        if (with_na) {
            const double u = r.unit();
            if (u < 0.01) s = na<double>();
            else if (u < 0.015) hh = na<double>();
            else if (u < 0.02) ll = na<double>();
            else if (u < 0.025) cc = na<double>();
        }
        b.src.push_back(s);
        b.high.push_back(hh);
        b.low.push_back(ll);
        b.close.push_back(cc);
    }
    return b;
}

static double span_of(bool use_true_range, double high, double low, double prev_close) {
    if (!use_true_range) return high - low;   // (high - low); na if either is
    if (is_na(high) || is_na(low) || is_na(prev_close)) return na<double>();
    return std::max(high - low, std::max(std::abs(high - prev_close), std::abs(low - prev_close)));
}

static void run_case(const Bars& b, int length, double mult, bool use_true_range, const char* label) {
    ta::KC kc(length, mult, use_true_range);
    ta::KCW kcw(length, mult, use_true_range);
    ta::EMA basis_ref(length), range_ref(length);
    double prev_close = na<double>();
    int bad = 0, bad_w = 0, band_values = 0;
    for (int t = 0; t < (int)b.src.size(); ++t) {
        const ta::KCResult got = kc.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double width = kcw.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double basis = basis_ref.compute(b.src[t]);
        const double span = span_of(use_true_range, b.high[t], b.low[t], prev_close);
        prev_close = b.close[t];
        const double range_ema = range_ref.compute(span);
        const double upper = basis + range_ema * mult;
        const double lower = basis - range_ema * mult;
        const double want_w = (upper - lower) / basis;
        if (!is_na(upper)) ++band_values;
        if (!same(got.middle, basis) || !same(got.upper, upper) || !same(got.lower, lower)) {
            if (bad++ < 3)
                std::printf("  [%s tr=%d] bar %d (%.17g %.17g %.17g) want (%.17g %.17g %.17g)\n",
                            label, int(use_true_range), t, got.middle, got.upper, got.lower,
                            basis, upper, lower);
        }
        if (!same(width, want_w) && bad_w++ < 3)
            std::printf("  [%s tr=%d] bar %d width %.17g want %.17g\n", label,
                        int(use_true_range), t, width, want_w);
    }
    CHECK(bad == 0, "[%s len=%d mult=%g tr=%d] KC differs on %d bars", label, length, mult,
          int(use_true_range), bad);
    CHECK(bad_w == 0, "[%s len=%d mult=%g tr=%d] KCW differs on %d bars", label, length, mult,
          int(use_true_range), bad_w);
    CHECK(band_values > 0, "[%s] no band values", label);
}

static void test_against_reference() {
    std::printf("test_against_reference\n");
    const Bars clean = make_bars(2000, 0x7c01ULL, false);
    const Bars dirty = make_bars(2000, 0x7c02ULL, true);
    for (bool tr : {true, false}) {
        for (int length : {1, 5, 20, 24}) {
            for (double mult : {1.5, 1.7, 4.0}) {
                run_case(clean, length, mult, tr, "clean");
                run_case(dirty, length, mult, tr, "dirty");
            }
        }
    }
}

// The flag changes the bands where a gap makes the true range exceed
// high - low, and nowhere else; the high - low range has a value on bar 0.
static void test_flag_effect() {
    std::printf("test_flag_effect\n");
    const Bars b = make_bars(500, 0x7c03ULL, false);
    ta::KC tr(10, 2.0, true), hl(10, 2.0, false);
    int differ = 0;
    for (int t = 0; t < (int)b.src.size(); ++t) {
        const ta::KCResult a = tr.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const ta::KCResult c = hl.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        CHECK(same(a.middle, c.middle), "bar %d middle differs with the flag", t);
        if (t == 0) {
            CHECK(is_na(a.upper) && !is_na(c.upper),
                  "bar 0: true-range band %.17g, high-low band %.17g", a.upper, c.upper);
        } else if (!same(a.upper, c.upper)) {
            ++differ;
        }
    }
    CHECK(differ > 0, "the flag never moved a band");
}

// Omitting the flag is useTrueRange = true, value for value.
static void test_default_is_true_range() {
    std::printf("test_default_is_true_range\n");
    const Bars b = make_bars(800, 0x7c04ULL, true);
    ta::KC plain(14, 1.5), spelled(14, 1.5, true);
    ta::KCW plain_w(14, 1.5), spelled_w(14, 1.5, true);
    int bad = 0;
    for (int t = 0; t < (int)b.src.size(); ++t) {
        const ta::KCResult a = plain.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const ta::KCResult c = spelled.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double wa = plain_w.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double wc = spelled_w.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        if (!same(a.middle, c.middle) || !same(a.upper, c.upper) || !same(a.lower, c.lower) ||
            !same(wa, wc))
            ++bad;
    }
    CHECK(bad == 0, "KC(14, 1.5) differs from KC(14, 1.5, true) on %d bars", bad);
}

// compute(first tick) -> recompute(second tick) -> recompute(final bar).
static void test_recompute_sequences() {
    std::printf("test_recompute_sequences\n");
    const Bars b = make_bars(1200, 0x7c05ULL, true);
    for (bool use_tr : {true, false}) {
        ta::KC kc(9, 1.7, use_tr);
        ta::KCW kcw(9, 1.7, use_tr);
        ta::EMA basis_ref(9), range_ref(9);
        Rng r(0x55ULL);
        double prev_close = na<double>();
        int bad = 0;
        for (int t = 0; t < (int)b.src.size(); ++t) {
            kc.compute(3100.0 + r.unit(), 3150.0, 3050.0, 3120.0);
            kcw.compute(3100.0 + r.unit(), 3150.0, 3050.0, 3120.0);
            kc.recompute(r.unit() < 0.1 ? na<double>() : 3000.0, 3400.0,
                         r.unit() < 0.1 ? na<double>() : 2900.0, 3001.0);
            kcw.recompute(3000.0, 3400.0, 2900.0, r.unit() < 0.1 ? na<double>() : 3001.0);
            const ta::KCResult got = kc.recompute(b.src[t], b.high[t], b.low[t], b.close[t]);
            const double w = kcw.recompute(b.src[t], b.high[t], b.low[t], b.close[t]);
            const double basis = basis_ref.compute(b.src[t]);
            const double span = span_of(use_tr, b.high[t], b.low[t], prev_close);
            prev_close = b.close[t];
            const double range_ema = range_ref.compute(span);
            const double upper = basis + range_ema * 1.7, lower = basis - range_ema * 1.7;
            if (!same(got.middle, basis) || !same(got.upper, upper) || !same(got.lower, lower) ||
                !same(w, (upper - lower) / basis))
                ++bad;
        }
        CHECK(bad == 0, "[tr=%d] %d mismatching bars after recompute sequences", int(use_tr), bad);
    }
}

int main() {
    test_against_reference();
    test_flag_effect();
    test_default_is_true_range();
    test_recompute_sequences();
    std::printf("test_ta_kc_range: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
