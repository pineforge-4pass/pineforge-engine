/*
 * test_vwap_bands.cpp — unit tests for VWAP bands 3-tuple form.
 *
 * Verifies VWAPBandsResult matches hand-computed values within 1e-9
 * over 100 synthetic bars with an anchor reset mid-series.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <pineforge/ta.hpp>

using namespace pineforge;
using namespace pineforge::ta;

static bool near(double a, double b, double tol = 1e-9) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::fabs(a - b) < tol;
}

// Hand-computed VWAP bands reference: single-pass over a price+volume
// sequence, resetting at every day boundary.
struct BandsRef {
    double vwap;
    double upper;
    double lower;
};

static BandsRef hand_compute_bands(const std::vector<double>& src,
                                   const std::vector<double>& vol,
                                   const std::vector<int64_t>& ts,
                                   int bar,
                                   double stdev_mult) {
    // Recompute from scratch using the same daily-anchor logic.
    double cum_pv = 0.0, cum_vol = 0.0, cum_pv_sq = 0.0;
    int64_t anchor_day = std::numeric_limits<int64_t>::min();
    BandsRef result{na<double>(), na<double>(), na<double>()};

    for (int i = 0; i <= bar; ++i) {
        if (is_na(src[i]) || is_na(vol[i])) continue;
        int64_t day = ts[i] / 86400000LL;
        if (anchor_day == std::numeric_limits<int64_t>::min()) {
            anchor_day = day;
        } else if (day != anchor_day) {
            cum_pv = 0.0;
            cum_vol = 0.0;
            cum_pv_sq = 0.0;
            anchor_day = day;
        }
        cum_pv += src[i] * vol[i];
        cum_pv_sq += src[i] * src[i] * vol[i];
        cum_vol += vol[i];
        if (i == bar) {
            if (cum_vol == 0.0) break;
            double mean = cum_pv / cum_vol;
            double variance = cum_pv_sq / cum_vol - mean * mean;
            if (variance < 0.0) variance = 0.0;
            double stdev = std::sqrt(variance);
            double offset = stdev_mult * stdev;
            result = {mean, mean + offset, mean - offset};
        }
    }
    return result;
}

static void test_basic_single_day() {
    // 10 bars on the same day; price increases linearly, uniform volume.
    constexpr int N = 10;
    constexpr double stdev_mult = 2.0;
    int64_t base_ts = 1704067200000LL;  // 2024-01-01 00:00:00 UTC (ms)

    std::vector<double> prices = {100, 101, 102, 103, 104, 105, 106, 107, 108, 109};
    std::vector<double> volumes(N, 1000.0);
    std::vector<int64_t> timestamps;
    for (int i = 0; i < N; ++i)
        timestamps.push_back(base_ts + (int64_t)i * 900000LL);  // 15-minute bars

    ta::VWAP vwap;

    for (int i = 0; i < N; ++i) {
        VWAPBandsResult r = vwap.compute_bands(prices[i], volumes[i], timestamps[i], stdev_mult);
        BandsRef ref = hand_compute_bands(prices, volumes, timestamps, i, stdev_mult);

        assert(near(r.vwap,  ref.vwap,  1e-9));
        assert(near(r.upper, ref.upper, 1e-9));
        assert(near(r.lower, ref.lower, 1e-9));
    }
    printf("test_basic_single_day: PASS\n");
}

static void test_anchor_reset_resets_cum_pv_sq() {
    // 5 bars on day 1, then 5 bars on day 2. After anchor reset, the
    // variance should reflect only day-2 bars.
    constexpr double stdev_mult = 1.5;
    int64_t day1_base = 1704067200000LL;  // 2024-01-01 00:00:00 UTC
    int64_t day2_base = day1_base + 86400000LL;  // 2024-01-02 00:00:00 UTC

    std::vector<double> prices  = {100, 102, 98, 101, 103, 200, 205, 195, 202, 198};
    std::vector<double> volumes = {  1,   2,  3,   2,   1,   5,   5,   5,   5,   5};
    std::vector<int64_t> timestamps;
    for (int i = 0; i < 5; ++i)
        timestamps.push_back(day1_base + (int64_t)i * 900000LL);
    for (int i = 0; i < 5; ++i)
        timestamps.push_back(day2_base + (int64_t)i * 900000LL);

    ta::VWAP vwap;

    for (int i = 0; i < 10; ++i) {
        VWAPBandsResult r = vwap.compute_bands(prices[i], volumes[i], timestamps[i], stdev_mult);
        BandsRef ref = hand_compute_bands(prices, volumes, timestamps, i, stdev_mult);

        assert(near(r.vwap,  ref.vwap,  1e-9));
        assert(near(r.upper, ref.upper, 1e-9));
        assert(near(r.lower, ref.lower, 1e-9));
    }
    printf("test_anchor_reset_resets_cum_pv_sq: PASS\n");
}

static void test_recompute_bands_restores_state() {
    // Verify recompute_bands restores state to just before the last compute call.
    // Compute bar 0, then compute bar 1 twice (via recompute) — should be identical.
    int64_t base_ts = 1704067200000LL;

    ta::VWAP vwap;
    VWAPBandsResult r0  = vwap.compute_bands(100.0, 1000.0, base_ts,                1.0);
    VWAPBandsResult r1a = vwap.compute_bands(102.0, 1000.0, base_ts + 900000LL,     1.0);
    VWAPBandsResult r1b = vwap.recompute_bands(102.0, 1000.0, base_ts + 900000LL,   1.0);

    assert(near(r1a.vwap,  r1b.vwap,  1e-15));
    assert(near(r1a.upper, r1b.upper, 1e-15));
    assert(near(r1a.lower, r1b.lower, 1e-15));

    (void)r0;
    printf("test_recompute_bands_restores_state: PASS\n");
}

static void test_recompute_bands_resets_cum_pv_sq_on_anchor_change() {
    // The anchor resets on day 2 bar 0 (compute_bands). recompute_bands
    // on that same bar must also reset cum_pv_sq_ (not leak day-1 state).
    int64_t day1 = 1704067200000LL;
    int64_t day2 = day1 + 86400000LL;

    ta::VWAP vwap;
    // Day 1 bar
    vwap.compute_bands(100.0, 1000.0, day1, 2.0);
    // Day 2 bar — compute then recompute: must agree
    VWAPBandsResult r_c  = vwap.compute_bands(200.0, 1000.0, day2, 2.0);
    VWAPBandsResult r_rc = vwap.recompute_bands(200.0, 1000.0, day2, 2.0);

    // On day 2, first bar: only one data point, variance = 0, stdev = 0.
    // Upper == lower == vwap.
    assert(near(r_c.vwap,  200.0, 1e-9));
    assert(near(r_c.upper, 200.0, 1e-9));
    assert(near(r_c.lower, 200.0, 1e-9));
    assert(near(r_rc.vwap,  r_c.vwap,  1e-15));
    assert(near(r_rc.upper, r_c.upper, 1e-15));
    assert(near(r_rc.lower, r_c.lower, 1e-15));

    printf("test_recompute_bands_resets_cum_pv_sq_on_anchor_change: PASS\n");
}

static void test_100_bars_with_anchor_reset() {
    // 100 bars with a day boundary at bar 50. Verify every bar matches
    // hand-computed reference within 1e-9.
    constexpr int N = 100;
    constexpr double stdev_mult = 2.0;
    int64_t day1 = 1704067200000LL;
    int64_t day2 = day1 + 86400000LL;

    std::vector<double> prices(N), volumes(N);
    std::vector<int64_t> timestamps(N);

    for (int i = 0; i < N; ++i) {
        // Alternating prices to produce a non-trivial variance
        prices[i]  = 100.0 + (i % 7) * 1.5 - (i % 3) * 0.8;
        volumes[i] = 500.0 + (i % 13) * 50.0;
        timestamps[i] = (i < 50 ? day1 : day2) + (int64_t)(i % 50) * 900000LL;
    }

    ta::VWAP vwap;
    for (int i = 0; i < N; ++i) {
        VWAPBandsResult r   = vwap.compute_bands(prices[i], volumes[i], timestamps[i], stdev_mult);
        BandsRef        ref = hand_compute_bands(prices, volumes, timestamps, i, stdev_mult);

        if (!near(r.vwap,  ref.vwap,  1e-9) ||
            !near(r.upper, ref.upper, 1e-9) ||
            !near(r.lower, ref.lower, 1e-9)) {
            printf("FAIL at bar %d: vwap=%.15g (ref=%.15g) upper=%.15g (ref=%.15g) lower=%.15g (ref=%.15g)\n",
                   i, r.vwap, ref.vwap, r.upper, ref.upper, r.lower, ref.lower);
            assert(false);
        }
    }
    printf("test_100_bars_with_anchor_reset: PASS\n");
}

static void test_scalar_compute_unaffected() {
    // Confirm the existing scalar compute() still works correctly alongside
    // compute_bands() — they share the same internal state.
    int64_t base_ts = 1704067200000LL;
    ta::VWAP vwap;

    double v1 = vwap.compute(100.0, 1000.0, base_ts);
    assert(near(v1, 100.0, 1e-12));

    double v2 = vwap.compute(102.0, 1000.0, base_ts + 900000LL);
    // expected vwap = (100*1000 + 102*1000) / 2000 = 101
    assert(near(v2, 101.0, 1e-12));

    double v2r = vwap.recompute(102.0, 1000.0, base_ts + 900000LL);
    assert(near(v2, v2r, 1e-15));

    printf("test_scalar_compute_unaffected: PASS\n");
}

int main() {
    test_basic_single_day();
    test_anchor_reset_resets_cum_pv_sq();
    test_recompute_bands_restores_state();
    test_recompute_bands_resets_cum_pv_sq_on_anchor_change();
    test_100_bars_with_anchor_reset();
    test_scalar_compute_unaffected();
    printf("All VWAP bands tests passed.\n");
    return 0;
}
