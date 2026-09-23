/*
 * test_ta_alma_floor.cpp — ta::ALMA's floor input (R5 lane TA1).
 *
 * TradingView's Pine v6 reference for ta.alma(series, length, offset, sigma,
 * floor): floor "An optional parameter. Specifies whether the offset
 * calculation is floored before ALMA is calculated. Default value is false."
 * Its own spelled-out version:
 *
 *     pine_alma(series, windowsize, offset, sigma) =>
 *         m = offset * (windowsize - 1)
 *         //m = math.floor(offset * (windowsize - 1)) // Used as m when math.floor=true
 *         s = windowsize / sigma
 *         norm = 0.0
 *         sum = 0.0
 *         for i = 0 to windowsize - 1
 *             weight = math.exp(-1 * math.pow(i - m, 2) / (2 * math.pow(s, 2)))
 *             norm := norm + weight
 *             sum := sum + series[windowsize - i - 1] * weight
 *         sum / norm
 *
 * and "na values in the source series are included in calculations and will
 * produce an na result."
 *
 * Witness: ALMA(length, offset, sigma, floor) against that recomputation --
 * the window re-read on every bar, one rounded operation per statement (this
 * TU builds with -ffp-contract=off, as the library does) -- bit for bit, for
 * floor false and true over lengths, offsets and sigmas where the floor moves
 * m and where it does not, through the warm-up, na sources, and compute ->
 * recompute -> recompute sequences. The three-argument constructor (floor
 * omitted) is floor = false, value for value.
 */

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

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

// The reference's pine_alma on bar t of `series` (history before bar 0 is na).
static double reference(const std::vector<double>& series, int t, int windowsize,
                        double offset, double sigma, bool floor) {
    double m = offset * (windowsize - 1);
    if (floor) m = std::floor(offset * (windowsize - 1));
    const double s = windowsize / sigma;
    double norm = 0.0;
    double sum = 0.0;
    for (int i = 0; i <= windowsize - 1; ++i) {
        const double d = i - m;
        const double num = -1 * std::pow(d, 2);
        const double den = 2 * std::pow(s, 2);
        const double weight = std::exp(num / den);
        norm = norm + weight;
        const int back = windowsize - i - 1;
        const double x = t - back >= 0 ? series[t - back] : na<double>();
        sum = sum + x * weight;
    }
    return sum / norm;
}

static std::vector<double> make_series(int n, uint64_t seed, bool with_na) {
    Rng r(seed);
    std::vector<double> v;
    double p = 2150.0;
    for (int i = 0; i < n; ++i) {
        p += (r.unit() - 0.5) * 9.0;
        v.push_back(with_na && r.unit() < 0.01 ? na<double>() : p);
    }
    return v;
}

struct Params { int length; double offset; double sigma; };

static const Params kParams[] = {
    {9, 0.85, 6.0},    // the reference example: m = 6.8, floored 6
    {14, 0.85, 6.0},   // the corpus probes' shape: m = 11.05, floored 11
    {31, 0.72, 5.4},   // m = 21.6, floored 21
    {9, 0.5, 6.0},     // m = 4 exactly: the floor changes nothing
    {10, 0.0, 3.0},    // m = 0
    {10, 1.0, 3.0},    // m = 9
    {2, 0.3, 1.0},     // m = 0.3, floored 0
    {1, 0.85, 6.0},    // a one-bar window: the source itself
    {50, 0.999, 0.5},  // m = 48.951, floored 48; a very narrow kernel
};

static void test_against_reference() {
    std::printf("test_against_reference\n");
    for (bool with_na : {false, true}) {
        const std::vector<double> series = make_series(1200, with_na ? 0xa11aULL : 0xa11bULL, with_na);
        for (const Params& p : kParams) {
            for (bool floor : {false, true}) {
                ta::ALMA alma(p.length, p.offset, p.sigma, floor);
                int bad = 0, valued = 0, first_value = -1;
                for (int t = 0; t < (int)series.size(); ++t) {
                    const double got = alma.compute(series[t]);
                    const double want = reference(series, t, p.length, p.offset, p.sigma, floor);
                    if (!is_na(want)) { ++valued; if (first_value < 0) first_value = t; }
                    if (!same(got, want) && bad++ < 3) {
                        std::printf("  [len=%d off=%g sig=%g floor=%d na=%d] bar %d: got %.17g want %.17g\n",
                                    p.length, p.offset, p.sigma, int(floor), int(with_na), t, got, want);
                    }
                }
                CHECK(bad == 0, "[len=%d off=%g sig=%g floor=%d na=%d] %d mismatching bars",
                      p.length, p.offset, p.sigma, int(floor), int(with_na), bad);
                // Warm-up: the first value is on bar length - 1 on a clean series.
                if (!with_na) {
                    CHECK(first_value == p.length - 1, "[len=%d] first value on bar %d",
                          p.length, first_value);
                    CHECK(valued == (int)series.size() - p.length + 1, "[len=%d] %d values",
                          p.length, valued);
                } else {
                    CHECK(valued > 0 && valued < (int)series.size() - p.length + 1,
                          "[len=%d na] %d values", p.length, valued);
                }
            }
        }
    }
}

// The floor changes the value exactly when it changes m.
static void test_floor_moves_value_only_when_it_moves_m() {
    std::printf("test_floor_moves_value_only_when_it_moves_m\n");
    const std::vector<double> series = make_series(300, 0xa11cULL, false);
    for (const Params& p : kParams) {
        ta::ALMA plain(p.length, p.offset, p.sigma, false);
        ta::ALMA floored(p.length, p.offset, p.sigma, true);
        const double m = p.offset * (p.length - 1);
        const bool moves = std::floor(m) != m;
        int differ = 0, valued = 0;
        for (double x : series) {
            const double a = plain.compute(x);
            const double b = floored.compute(x);
            if (is_na(a)) continue;
            ++valued;
            if (!same(a, b)) ++differ;
        }
        if (moves) {
            CHECK(differ > 0, "[len=%d off=%g] floor moved m but no value", p.length, p.offset);
        } else {
            CHECK(differ == 0, "[len=%d off=%g] floor left m alone but moved %d values",
                  p.length, p.offset, differ);
        }
        CHECK(valued > 0, "[len=%d] no values", p.length);
    }
}

// Omitting floor is floor = false, value for value, and an explicit
// three-argument instance never sees the floored window.
static void test_default_is_unfloored() {
    std::printf("test_default_is_unfloored\n");
    const std::vector<double> series = make_series(600, 0xa11dULL, true);
    for (const Params& p : kParams) {
        ta::ALMA three(p.length, p.offset, p.sigma);
        ta::ALMA four(p.length, p.offset, p.sigma, false);
        int bad = 0;
        for (double x : series) {
            if (!same(three.compute(x), four.compute(x))) ++bad;
        }
        CHECK(bad == 0, "[len=%d] default differs from floor=false on %d bars", p.length, bad);
    }
    ta::ALMA defaults(9);   // offset 0.85, sigma 6, floor false
    ta::ALMA spelled(9, 0.85, 6.0, false);
    int bad = 0;
    for (double x : series) {
        if (!same(defaults.compute(x), spelled.compute(x))) ++bad;
    }
    CHECK(bad == 0, "ALMA(9) differs from ALMA(9, 0.85, 6, false) on %d bars", bad);
}

// compute(first tick) -> recompute(second tick) -> recompute(final value):
// the answer is the reference over the final values, on this bar and later.
static void test_recompute_sequences() {
    std::printf("test_recompute_sequences\n");
    const std::vector<double> series = make_series(700, 0xa11eULL, true);
    for (const Params& p : kParams) {
        for (bool floor : {false, true}) {
            ta::ALMA alma(p.length, p.offset, p.sigma, floor);
            Rng r(0x77ULL);
            int bad = 0;
            for (int t = 0; t < (int)series.size(); ++t) {
                alma.compute(2000.0 + r.unit() * 300.0);
                alma.recompute(r.unit() < 0.1 ? na<double>() : 1900.0 + r.unit() * 300.0);
                const double got = alma.recompute(series[t]);
                const double want = reference(series, t, p.length, p.offset, p.sigma, floor);
                if (!same(got, want)) ++bad;
            }
            CHECK(bad == 0, "[len=%d off=%g floor=%d] %d mismatching bars after recompute",
                  p.length, p.offset, int(floor), bad);
        }
    }
    // A recompute before any compute is a compute.
    ta::ALMA early(3, 0.85, 6.0, true), fresh(3, 0.85, 6.0, true);
    CHECK(same(early.recompute(7.0), fresh.compute(7.0)), "recompute before the first compute");
}

int main() {
    test_against_reference();
    test_floor_moves_value_only_when_it_moves_m();
    test_default_is_unfloored();
    test_recompute_sequences();
    std::printf("test_ta_alma_floor: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
