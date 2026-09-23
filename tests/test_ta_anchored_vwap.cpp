/*
 * test_ta_anchored_vwap.cpp — ta::AnchoredVWAP / ta::AnchoredVWAPBands, the
 * VWAP whose accumulation restarts on every bar a caller-supplied anchor is
 * true (R5 lane TA1).
 *
 * The rule, from TradingView's Pine v6 reference for ta.vwap:
 *   anchor: "The condition that triggers the reset of VWAP calculations. When
 *     true, calculations reset; when false, calculations proceed using the
 *     values accumulated since the previous reset."
 *   Remarks: "Calculations only begin the first time the anchor condition
 *     becomes true. Until then, the function returns na."
 *   stdev_mult: "The upper_band/lower_band values are calculated using the
 *     VWAP to which the standard deviation multiplied by this argument is
 *     added/subtracted."
 * The band arithmetic is the one ta::VWAP already has (the volume-weighted
 * variance sum(src*src*vol)/sum(vol) - vwap*vwap, floored at 0), pinned by
 * the vwap-bands corpus probes; the na rule is ta::VWAP's too (an na source
 * or volume answers na and does not enter the sums), and an anchor on such a
 * bar still resets them.
 *
 * Witness: every value is compared bit for bit against an independent naive
 * recomputation -- for each bar, the sums re-added from the last anchored
 * bar, one rounded operation at a time (this TU builds with
 * -ffp-contract=off, as the library does) -- over anchors on bar 0 only,
 * never, on every bar, at random, on na bars and on zero-volume bars; through
 * compute -> recompute -> recompute sequences that also flip the anchor inside
 * the bar; and against ta::VWAP itself, whose session-day reset is exactly an
 * anchor on the bar the UTC day changes.
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

// Bit-identical, or both na.
static bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return std::memcmp(&a, &b, sizeof a) == 0;
}

static bool same(const ta::VWAPBandsResult& a, const ta::VWAPBandsResult& b) {
    return same(a.vwap, b.vwap) && same(a.upper, b.upper) && same(a.lower, b.lower);
}

// Deterministic generator (xorshift64*), so every run sees the same series.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

struct Feed {
    std::vector<double> src, vol;
    std::vector<int64_t> ts;
};

// A 15-minute random walk over several UTC days, with na sources, na volumes
// and zero volumes sprinkled in.
static Feed make_feed(int n, uint64_t seed, bool with_na) {
    Rng r(seed);
    Feed f;
    double price = 1850.0;
    const int64_t t0 = 1704067200000LL + 37LL * 900000LL;   // 2024-01-01 09:15 UTC
    for (int i = 0; i < n; ++i) {
        price += (r.unit() - 0.5) * 6.0;
        double v = (std::floor(r.unit() * 4000.0) + 1.0) / 8.0;   // > 0 on a clean feed
        double s = price + (r.unit() - 0.5);
        if (with_na) {
            double u = r.unit();
            if (u < 0.02) s = na<double>();
            else if (u < 0.04) v = na<double>();
            else if (u < 0.07) v = 0.0;
        }
        f.src.push_back(s);
        f.vol.push_back(v);
        f.ts.push_back(t0 + int64_t(i) * 900000LL);
    }
    return f;
}

// The naive reference: the sums re-added from the last anchored bar.
static ta::VWAPBandsResult reference(const Feed& f, const std::vector<char>& anchor,
                                     int t, double mult) {
    const ta::VWAPBandsResult none{na<double>(), na<double>(), na<double>()};
    int start = -1;
    for (int k = t; k >= 0; --k) {
        if (anchor[k]) { start = k; break; }
    }
    if (start < 0) return none;                          // no anchor yet
    if (is_na(f.src[t]) || is_na(f.vol[t])) return none; // na bar
    double pv = 0.0, pvv = 0.0, v = 0.0;
    for (int k = start; k <= t; ++k) {
        if (is_na(f.src[k]) || is_na(f.vol[k])) continue;
        const double x = f.src[k];
        const double w = f.vol[k];
        const double xw = x * w;
        pv = pv + xw;
        const double xx = x * x;
        const double xxw = xx * w;
        pvv = pvv + xxw;
        v = v + w;
    }
    if (v == 0.0) return none;
    const double mean = pv / v;
    const double second = pvv / v;
    const double mean_sq = mean * mean;
    double variance = second - mean_sq;
    if (variance < 0.0) variance = 0.0;
    const double sd = std::sqrt(variance);
    const double offset = mult * sd;
    return {mean, mean + offset, mean - offset};
}

static std::vector<char> anchors_pattern(int n, const char* kind, uint64_t seed,
                                         const Feed* f = nullptr) {
    std::vector<char> a(n, 0);
    Rng r(seed);
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(kind, "bar0") == 0) a[i] = i == 0;
        else if (std::strcmp(kind, "never") == 0) a[i] = 0;
        else if (std::strcmp(kind, "every") == 0) a[i] = 1;
        else if (std::strcmp(kind, "random") == 0) a[i] = i >= 37 && r.unit() < 0.04;
        else if (std::strcmp(kind, "late") == 0) a[i] = i == n - 5;
        else if (std::strcmp(kind, "day") == 0)
            a[i] = i > 0 && f->ts[i] / 86400000LL != f->ts[i - 1] / 86400000LL;
        else if (std::strcmp(kind, "day_bar0") == 0)
            a[i] = i == 0 || f->ts[i] / 86400000LL != f->ts[i - 1] / 86400000LL;
    }
    if (std::strcmp(kind, "random") == 0) a[37] = 1;
    return a;
}

// One pass over a feed: compute() and compute_bands() on two instances, both
// against the reference; returns the number of bars with a value.
static int check_pass(const Feed& f, const std::vector<char>& anchor, double mult,
                      const char* label) {
    ta::AnchoredVWAP single;
    ta::AnchoredVWAP bands;
    ta::AnchoredVWAPBands wrapper(mult);
    int valued = 0, bad = 0;
    for (int t = 0; t < (int)f.src.size(); ++t) {
        const ta::VWAPBandsResult want = reference(f, anchor, t, mult);
        const double got = single.compute(f.src[t], f.vol[t], anchor[t] != 0);
        const ta::VWAPBandsResult got_b =
            bands.compute_bands(f.src[t], f.vol[t], anchor[t] != 0, mult);
        const ta::VWAPBandsResult got_w = wrapper.compute(f.src[t], f.vol[t], anchor[t] != 0);
        if (!is_na(want.vwap)) ++valued;
        if (!same(got, want.vwap) || !same(got_b, want) || !same(got_w, want)) {
            if (bad++ < 3) {
                std::printf("  [%s mult=%g] bar %d: got %.17g / (%.17g %.17g %.17g), "
                            "want %.17g (%.17g %.17g)\n", label, mult, t, got,
                            got_b.vwap, got_b.upper, got_b.lower, want.vwap,
                            want.upper, want.lower);
            }
        }
    }
    CHECK(bad == 0, "[%s mult=%g] %d mismatching bars", label, mult, bad);
    return valued;
}

static void test_anchor_patterns() {
    std::printf("test_anchor_patterns\n");
    const Feed clean = make_feed(3000, 0x5eed1ULL, false);
    const Feed dirty = make_feed(3000, 0x5eed2ULL, true);
    for (const Feed* f : {&clean, &dirty}) {
        const int n = (int)f->src.size();
        const bool is_dirty = f == &dirty;
        for (double mult : {0.0, 1.0, 2.5, -1.5}) {
            const int v0 = check_pass(*f, anchors_pattern(n, "bar0", 1), mult,
                                      is_dirty ? "dirty/bar0" : "clean/bar0");
            const int v1 = check_pass(*f, anchors_pattern(n, "never", 1), mult,
                                      is_dirty ? "dirty/never" : "clean/never");
            const int v2 = check_pass(*f, anchors_pattern(n, "every", 1), mult,
                                      is_dirty ? "dirty/every" : "clean/every");
            const int v3 = check_pass(*f, anchors_pattern(n, "random", 7), mult,
                                      is_dirty ? "dirty/random" : "clean/random");
            const int v4 = check_pass(*f, anchors_pattern(n, "late", 1), mult,
                                      is_dirty ? "dirty/late" : "clean/late");
            const int v5 = check_pass(*f, anchors_pattern(n, "day", 1, f), mult,
                                      is_dirty ? "dirty/day" : "clean/day");
            // The patterns reach what they claim to reach.
            CHECK(v1 == 0, "never-anchored VWAP answered %d values", v1);
            CHECK(v4 > 0 && v4 <= 5, "late anchor answered %d values", v4);
            if (!is_dirty) {
                CHECK(v0 == n, "bar-0 anchor on a clean feed answered %d of %d", v0, n);
                CHECK(v2 == n, "every-bar anchor on a clean feed answered %d of %d", v2, n);
                CHECK(v3 == n - 37, "random anchors answered %d, want %d", v3, n - 37);
                CHECK(v5 > 0 && v5 < n, "day anchors answered %d", v5);
            }
        }
    }
    // A na multiplier leaves the VWAP itself alone and the bands na.
    ta::AnchoredVWAP a;
    const ta::VWAPBandsResult r = a.compute_bands(100.0, 2.0, true, na<double>());
    CHECK(same(r.vwap, 100.0) && is_na(r.upper) && is_na(r.lower),
          "na stdev_mult -> (%.17g %.17g %.17g)", r.vwap, r.upper, r.lower);
}

static void test_na_and_zero_volume_anchor_bars() {
    std::printf("test_na_and_zero_volume_anchor_bars\n");
    // Hand-built: the anchor lands on an na-source bar, an na-volume bar and
    // a zero-volume bar; each resets the sums and contributes nothing.
    Feed f;
    const double src[] = {10, 11, na<double>(), 13, 14, 15, 16, 17, 18};
    const double vol[] = {1, 2, 3, 4, na<double>(), 6, 0, 8, 9};
    const char anc[] = {0, 1, 1, 0, 1, 0, 1, 0, 0};
    for (int i = 0; i < 9; ++i) {
        f.src.push_back(src[i]);
        f.vol.push_back(vol[i]);
        f.ts.push_back(int64_t(i) * 900000LL);
    }
    const std::vector<char> anchor(anc, anc + 9);
    ta::AnchoredVWAP v;
    std::vector<double> got;
    for (int t = 0; t < 9; ++t) got.push_back(v.compute(f.src[t], f.vol[t], anchor[t] != 0));
    // bar 0: before the first anchor; bar 1: 11; bar 2: anchored na source ->
    // na and empty sums; bar 3: 13 alone; bar 4: anchored na volume -> na;
    // bar 5: 15 alone; bar 6: anchored zero volume -> 0/0 = na; bar 7: 17
    // (the zero-volume bar weighs nothing); bar 8: (17*8 + 18*9) / 17.
    CHECK(is_na(got[0]), "bar 0 %.17g", got[0]);
    CHECK(same(got[1], 11.0), "bar 1 %.17g", got[1]);
    CHECK(is_na(got[2]), "bar 2 %.17g", got[2]);
    CHECK(same(got[3], 13.0), "bar 3 %.17g", got[3]);
    CHECK(is_na(got[4]), "bar 4 %.17g", got[4]);
    CHECK(same(got[5], 15.0), "bar 5 %.17g", got[5]);
    CHECK(is_na(got[6]), "bar 6 %.17g", got[6]);
    CHECK(same(got[7], 17.0), "bar 7 %.17g", got[7]);
    CHECK(same(got[8], (17.0 * 8.0 + 18.0 * 9.0) / 17.0), "bar 8 %.17g", got[8]);
    check_pass(f, anchor, 1.0, "hand-built");
}

// compute(first tick) -> recompute(second tick) -> recompute(final values):
// the bar's answer, and every later bar's, must be the reference over the
// final values -- whatever the discarded ticks said, anchor flag included.
static void test_recompute_sequences() {
    std::printf("test_recompute_sequences\n");
    const Feed f = make_feed(1500, 0x5eed3ULL, true);
    const int n = (int)f.src.size();
    for (const char* kind : {"random", "every", "bar0", "never"}) {
        const std::vector<char> anchor = anchors_pattern(n, kind, 11);
        Rng r(0xabcULL);
        ta::AnchoredVWAP single;
        ta::AnchoredVWAPBands bands(2.0);
        int bad = 0;
        for (int t = 0; t < n; ++t) {
            // Discarded ticks: other prices, volumes and anchor flags.
            const double s1 = 1800.0 + r.unit() * 100.0, v1 = r.unit() * 500.0;
            const double s2 = r.unit() < 0.1 ? na<double>() : 1700.0 + r.unit() * 300.0;
            const double v2 = r.unit() < 0.1 ? 0.0 : r.unit() * 800.0;
            const bool a1 = r.unit() < 0.5, a2 = r.unit() < 0.5;
            single.compute(s1, v1, a1);
            bands.compute(s1, v1, a1);
            single.recompute(s2, v2, a2);
            bands.recompute(s2, v2, a2);
            const double got = single.recompute(f.src[t], f.vol[t], anchor[t] != 0);
            const ta::VWAPBandsResult got_b = bands.recompute(f.src[t], f.vol[t], anchor[t] != 0);
            const ta::VWAPBandsResult want = reference(f, anchor, t, 2.0);
            if (!same(got, want.vwap) || !same(got_b, want)) {
                if (bad++ < 3) {
                    std::printf("  [%s] bar %d: got %.17g want %.17g\n", kind, t, got, want.vwap);
                }
            }
        }
        CHECK(bad == 0, "[%s] %d mismatching bars after recompute sequences", kind, bad);
    }
    // A recompute before any compute is the pristine state's compute.
    ta::AnchoredVWAP fresh, early;
    CHECK(same(early.recompute(5.0, 2.0, true), fresh.compute(5.0, 2.0, true)),
          "recompute before the first compute");
    ta::AnchoredVWAP fresh2, early2;
    CHECK(same(early2.recompute(5.0, 2.0, false), fresh2.compute(5.0, 2.0, false)),
          "unanchored recompute before the first compute");
}

// ta::VWAP resets when the UTC day changes (its tz-less overloads). An anchor
// raised on exactly those bars reproduces it bit for bit -- on every bar when
// bar 0 is anchored too, and from the first day change on when it is not
// (before it the anchored form is na, ta::VWAP is not).
static void test_session_day_equivalence() {
    std::printf("test_session_day_equivalence\n");
    for (bool with_na : {false, true}) {
        const Feed f = make_feed(4000, with_na ? 0x5eed4ULL : 0x5eed5ULL, with_na);
        const int n = (int)f.src.size();
        const std::vector<char> day0 = anchors_pattern(n, "day_bar0", 1, &f);
        const std::vector<char> day = anchors_pattern(n, "day", 1, &f);
        int first_change = -1;
        for (int t = 0; t < n; ++t) if (day[t]) { first_change = t; break; }
        CHECK(first_change > 0, "the feed crosses a UTC day (%d)", first_change);
        ta::VWAP daily, daily_b;
        ta::VWAPBands daily_w(1.5);
        ta::AnchoredVWAP a0, a0_b, a1;
        ta::AnchoredVWAPBands a0_w(1.5);
        int bad0 = 0, bad1 = 0, pre = 0;
        for (int t = 0; t < n; ++t) {
            const double d = daily.compute(f.src[t], f.vol[t], f.ts[t]);
            const ta::VWAPBandsResult db = daily_b.compute_bands(f.src[t], f.vol[t], f.ts[t], 1.5);
            const ta::VWAPBandsResult dw = daily_w.compute(f.src[t], f.vol[t], f.ts[t]);
            const double x0 = a0.compute(f.src[t], f.vol[t], day0[t] != 0);
            const ta::VWAPBandsResult x0b = a0_b.compute_bands(f.src[t], f.vol[t], day0[t] != 0, 1.5);
            const ta::VWAPBandsResult x0w = a0_w.compute(f.src[t], f.vol[t], day0[t] != 0);
            const double x1 = a1.compute(f.src[t], f.vol[t], day[t] != 0);
            if (!same(d, x0) || !same(db, x0b) || !same(dw, x0w)) ++bad0;
            if (t < first_change) {
                if (!is_na(x1)) ++pre;
            } else if (!same(d, x1)) {
                ++bad1;
            }
        }
        CHECK(bad0 == 0, "[na=%d] anchored-at-day-changes-and-bar-0 differs from ta::VWAP on %d bars",
              int(with_na), bad0);
        CHECK(bad1 == 0, "[na=%d] anchored-at-day-changes differs from ta::VWAP after the first "
              "change on %d bars", int(with_na), bad1);
        CHECK(pre == 0, "[na=%d] %d values before the first anchor", int(with_na), pre);
    }
}

int main() {
    test_anchor_patterns();
    test_na_and_zero_volume_anchor_bars();
    test_recompute_sequences();
    test_session_day_equivalence();
    std::printf("test_ta_anchored_vwap: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
