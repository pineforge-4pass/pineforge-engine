/*
 * test_ta_kc_basis.cpp — ta::KC's middle band is ta::EMA of its source on
 * every bar (R5 lane TA1).
 *
 * TradingView's Pine v6 reference spells ta.kc out as
 *
 *     f_kc(src, length, mult, useTrueRange) =>
 *         float basis = ta.ema(src, length)
 *         float span = (useTrueRange) ? ta.tr : (high - low)
 *         float rangeEma = ta.ema(span, length)
 *         [basis, basis + rangeEma * mult, basis - rangeEma * mult]
 *
 * and ta.kcw as ((basis + rangeEma * mult) - (basis - rangeEma * mult)) /
 * basis. The bare ta.tr is "True range, equivalent to ta.tr(handle_na =
 * false)", whose handle_na reads "If false, it returns na" when "the previous
 * bar's close is na" -- so on the first bar the span is na, rangeEma is na,
 * and the bands are na, but the basis is ta.ema(src, length), a value.
 * ta::KC answered na for its middle band wherever its range EMA was na (the
 * first bar, and a bar with an na high/low/close), although the middle band
 * is the basis alone.
 *
 * Witness: KC(length, mult) against that recomputation -- basis and range
 * EMAs from ta::EMA, the true range with handle_na = false -- bit for bit on
 * every bar: middle == ta::EMA(src) always, the bands na exactly where the
 * range EMA is, KCW == the reference width, over na sources and na
 * high/low/close, both EMA seedings (the ambient ema_na_warmup_flag() the
 * source layer may raise), and compute -> recompute -> recompute sequences.
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
    double c = 1500.0;
    for (int i = 0; i < n; ++i) {
        const double o = c;
        c = o + (r.unit() - 0.5) * 12.0;
        const double h = std::max(o, c) + r.unit() * 4.0;
        const double l = std::min(o, c) - r.unit() * 4.0;
        double s = (h + l + c) / 3.0;
        double hh = h, ll = l, cc = c;
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

// ta.tr(handle_na = false) against the previous bar's close.
static double true_range(double high, double low, double prev_close) {
    if (is_na(high) || is_na(low) || is_na(prev_close)) return na<double>();
    return std::max(high - low, std::max(std::abs(high - prev_close), std::abs(low - prev_close)));
}

static void run_case(const Bars& b, int length, double mult, bool na_warmup, const char* label) {
    const bool previous = ta::ema_na_warmup_flag();
    ta::ema_na_warmup_flag() = na_warmup;
    ta::KC kc(length, mult);
    ta::KCW kcw(length, mult);
    ta::EMA basis_ref(length);
    ta::EMA range_ref(length);
    double prev_close = na<double>();
    int bad_mid = 0, bad_band = 0, bad_width = 0, middle_values = 0, band_values = 0;
    for (int t = 0; t < (int)b.src.size(); ++t) {
        const ta::KCResult got = kc.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double width = kcw.compute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double basis = basis_ref.compute(b.src[t]);
        const double span = true_range(b.high[t], b.low[t], prev_close);
        prev_close = b.close[t];
        const double range_ema = range_ref.compute(span);
        const double upper = basis + range_ema * mult;
        const double lower = basis - range_ema * mult;
        const double want_width = (upper - lower) / basis;
        if (!is_na(basis)) ++middle_values;
        if (!is_na(upper)) ++band_values;
        if (!same(got.middle, basis)) {
            if (bad_mid++ < 3)
                std::printf("  [%s] bar %d middle %.17g, ta::EMA %.17g\n", label, t, got.middle, basis);
        }
        if (!same(got.upper, upper) || !same(got.lower, lower)) {
            if (bad_band++ < 3)
                std::printf("  [%s] bar %d bands (%.17g %.17g) want (%.17g %.17g)\n", label, t,
                            got.upper, got.lower, upper, lower);
        }
        if (!same(width, want_width)) {
            if (bad_width++ < 3)
                std::printf("  [%s] bar %d width %.17g want %.17g\n", label, t, width, want_width);
        }
    }
    ta::ema_na_warmup_flag() = previous;
    CHECK(bad_mid == 0, "[%s] middle band differs from ta::EMA on %d bars", label, bad_mid);
    CHECK(bad_band == 0, "[%s] bands differ from the reference on %d bars", label, bad_band);
    CHECK(bad_width == 0, "[%s] KCW differs from the reference on %d bars", label, bad_width);
    CHECK(middle_values > band_values, "[%s] the middle band leads the bands (%d vs %d values)",
          label, middle_values, band_values);
}

static void test_middle_is_the_basis() {
    std::printf("test_middle_is_the_basis\n");
    const Bars clean = make_bars(2500, 0x6c01ULL, false);
    const Bars dirty = make_bars(2500, 0x6c02ULL, true);
    for (int length : {1, 2, 5, 20, 24}) {
        for (double mult : {1.5, 1.7, 2.0}) {
            run_case(clean, length, mult, false, "clean");
            run_case(dirty, length, mult, false, "dirty");
            run_case(clean, length, mult, true, "clean/sma-seeded");
            run_case(dirty, length, mult, true, "dirty/sma-seeded");
        }
    }
}

// The first bar in the literal: the basis is the source (a first-value EMA
// seeds on it), the true range has no previous close, so the bands are na.
static void test_first_bar() {
    std::printf("test_first_bar\n");
    ta::KC kc(5, 2.0);
    const ta::KCResult r0 = kc.compute(100.0, 102.0, 97.0, 101.0);
    CHECK(same(r0.middle, 100.0), "bar 0 middle %.17g", r0.middle);
    CHECK(is_na(r0.upper) && is_na(r0.lower), "bar 0 bands (%.17g %.17g)", r0.upper, r0.lower);
    // Bar 1: the range EMA seeds on the first true range, max(4, |104-101|,
    // |100-101|) = 4; the basis is (1/3) * 103 + (2/3) * 100.
    const ta::KCResult r1 = kc.compute(103.0, 104.0, 100.0, 102.0);
    const double alpha = 2.0 / (5 + 1);
    const double basis = alpha * 103.0 + (1.0 - alpha) * 100.0;
    CHECK(same(r1.middle, basis), "bar 1 middle %.17g want %.17g", r1.middle, basis);
    CHECK(same(r1.upper, basis + 2.0 * 4.0), "bar 1 upper %.17g", r1.upper);
    CHECK(same(r1.lower, basis - 2.0 * 4.0), "bar 1 lower %.17g", r1.lower);
    ta::KCW kcw(5, 2.0);
    CHECK(is_na(kcw.compute(100.0, 102.0, 97.0, 101.0)), "bar 0 width");
}

// compute(first tick) -> recompute(second tick) -> recompute(final bar):
// the answers are the reference over the final bars.
static void test_recompute_sequences() {
    std::printf("test_recompute_sequences\n");
    const Bars b = make_bars(1500, 0x6c03ULL, true);
    ta::KC kc(14, 1.5);
    ta::EMA basis_ref(14), range_ref(14);
    Rng r(0x99ULL);
    double prev_close = na<double>();
    int bad = 0;
    for (int t = 0; t < (int)b.src.size(); ++t) {
        kc.compute(1500.0 + r.unit() * 10.0, 1510.0, 1490.0 + r.unit(), 1500.0);
        kc.recompute(r.unit() < 0.1 ? na<double>() : 1400.0 + r.unit() * 200.0,
                     1600.0, 1400.0, r.unit() < 0.1 ? na<double>() : 1450.0);
        const ta::KCResult got = kc.recompute(b.src[t], b.high[t], b.low[t], b.close[t]);
        const double basis = basis_ref.compute(b.src[t]);
        const double span = true_range(b.high[t], b.low[t], prev_close);
        prev_close = b.close[t];
        const double range_ema = range_ref.compute(span);
        if (!same(got.middle, basis) || !same(got.upper, basis + range_ema * 1.5) ||
            !same(got.lower, basis - range_ema * 1.5)) {
            if (bad++ < 3) std::printf("  bar %d middle %.17g want %.17g\n", t, got.middle, basis);
        }
    }
    CHECK(bad == 0, "%d mismatching bars after recompute sequences", bad);
}

int main() {
    test_middle_is_the_basis();
    test_first_bar();
    test_recompute_sequences();
    std::printf("test_ta_kc_basis: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
