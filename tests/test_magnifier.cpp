#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <pineforge/magnifier.hpp>

using namespace pineforge;

// ─── helpers ───────────────────────────────────────────────────────────────────

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);    \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) < tol;
}

// ─── ENDPOINTS distribution ──────────────────────────────────────────────────

static void test_bullish_4_endpoints() {
    std::printf("test_bullish_4_endpoints\n");
    // Tie between high and low distances => low-first path O -> L -> H -> C
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pts.size() == 4);
    CHECK(near(pts[0], 100.0));  // O
    CHECK(near(pts[1], 90.0));   // L (first turning point)
    CHECK(near(pts[2], 110.0));  // H (second turning point)
    CHECK(near(pts[3], 105.0));  // C
}

static void test_bearish_4_endpoints() {
    std::printf("test_bearish_4_endpoints\n");
    // Open is nearer high => high-first path O -> H -> L -> C
    Bar bar{105.0, 110.0, 90.0, 100.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pts.size() == 4);
    CHECK(near(pts[0], 105.0));  // O
    CHECK(near(pts[1], 110.0));  // H (first turning point)
    CHECK(near(pts[2], 90.0));   // L (second turning point)
    CHECK(near(pts[3], 100.0));  // C
}

static void test_doji_treated_as_bullish() {
    std::printf("test_doji_treated_as_bullish\n");
    // Tie between high and low distances => low-first path O -> L -> H -> C
    Bar bar{100.0, 110.0, 90.0, 100.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pts.size() == 4);
    CHECK(near(pts[0], 100.0));  // O
    CHECK(near(pts[1], 90.0));   // L
    CHECK(near(pts[2], 110.0));  // H
    CHECK(near(pts[3], 100.0));  // C
}


static void test_bullish_open_near_high_uses_high_first_path() {
    std::printf("test_bullish_open_near_high_uses_high_first_path\n");
    // Bullish bar, but open is closer to high than low:
    // |112 - 109| = 3, |109 - 90| = 19, so path must be O -> H -> L -> C.
    Bar bar{109.0, 112.0, 90.0, 110.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pts.size() == 4);
    CHECK(near(pts[0], 109.0));
    CHECK(near(pts[1], 112.0));
    CHECK(near(pts[2], 90.0));
    CHECK(near(pts[3], 110.0));
}

static void test_bearish_open_near_low_uses_low_first_path() {
    std::printf("test_bearish_open_near_low_uses_low_first_path\n");
    // Bearish bar, but open is closer to low than high:
    // |110 - 100| = 10, |100 - 94| = 6, so path must be O -> L -> H -> C.
    Bar bar{100.0, 110.0, 94.0, 96.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pts.size() == 4);
    CHECK(near(pts[0], 100.0));
    CHECK(near(pts[1], 94.0));
    CHECK(near(pts[2], 110.0));
    CHECK(near(pts[3], 96.0));
}

// ─── UNIFORM distribution ────────────────────────────────────────────────────

static void test_uniform_8_samples() {
    std::printf("test_uniform_8_samples\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 8, MagnifierDistribution::UNIFORM);
    CHECK(pts.size() == 8);
    CHECK(near(pts[0], 100.0));    // first = O
    CHECK(near(pts[7], 105.0));    // last = C
    // Monotonicity not required (path goes up and down),
    // but values should be within [low, high]
    for (auto p : pts) {
        CHECK(p >= 90.0 - 1e-9 && p <= 110.0 + 1e-9);
    }
}

// ─── COSINE distribution ─────────────────────────────────────────────────────

static void test_cosine_8_samples() {
    std::printf("test_cosine_8_samples\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 8, MagnifierDistribution::COSINE);
    CHECK(pts.size() == 8);
    CHECK(near(pts[0], 100.0));    // first = O
    CHECK(near(pts[7], 105.0));    // last = C
    for (auto p : pts) {
        CHECK(p >= 90.0 - 1e-9 && p <= 110.0 + 1e-9);
    }
}

// ─── Minimum samples ─────────────────────────────────────────────────────────

static void test_min_2_samples() {
    std::printf("test_min_2_samples\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 2, MagnifierDistribution::UNIFORM);
    CHECK(pts.size() == 2);
    CHECK(near(pts[0], 100.0));  // O
    CHECK(near(pts[1], 105.0));  // C
}

static void test_min_clamp_to_2() {
    std::printf("test_min_clamp_to_2\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 1, MagnifierDistribution::UNIFORM);
    CHECK(pts.size() == 2);
    CHECK(near(pts[0], 100.0));
    CHECK(near(pts[1], 105.0));
}

// ─── Degenerate bar ──────────────────────────────────────────────────────────

static void test_degenerate_bar() {
    std::printf("test_degenerate_bar\n");
    // All prices equal — should not crash
    Bar bar{100.0, 100.0, 100.0, 100.0, 0.0, 0};
    auto pts = sample_price_path(bar, 8, MagnifierDistribution::UNIFORM);
    CHECK(pts.size() == 8);
    for (auto p : pts) {
        CHECK(near(p, 100.0));
    }
    // Also test ENDPOINTS
    auto pts2 = sample_price_path(bar, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(pts2.size() == 4);
    for (auto p : pts2) {
        CHECK(near(p, 100.0));
    }
}

// ─── TRIANGLE distribution ───────────────────────────────────────────────────

static void test_triangle_8_samples() {
    std::printf("test_triangle_8_samples\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 8, MagnifierDistribution::TRIANGLE);
    CHECK(pts.size() == 8);
    CHECK(near(pts[0], 100.0));    // first = O
    CHECK(near(pts[7], 105.0));    // last = C
    for (auto p : pts) {
        CHECK(p >= 90.0 - 1e-9 && p <= 110.0 + 1e-9);
    }
}

// ─── FRONT_LOADED / BACK_LOADED distributions ────────────────────────────────

static void test_front_loaded_density_near_open() {
    std::printf("test_front_loaded_density_near_open\n");
    // Bullish bar; FRONT_LOADED should cluster samples near the open.
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 10, MagnifierDistribution::FRONT_LOADED);
    CHECK(pts.size() == 10);
    CHECK(near(pts[0], 100.0));     // first = O
    CHECK(near(pts[9], 105.0));     // last = C
    // Check monotonic-in-t density: the gap between the first two sample ts
    // must be smaller than the gap between the last two (quadratic easing).
    // Translating to prices: first gap along path is < last gap along path.
    // We approximate by comparing the path distance between pts[0..1] vs
    // pts[8..9].
    double front_gap = std::fabs(pts[1] - pts[0]);
    double back_gap  = std::fabs(pts[9] - pts[8]);
    CHECK(front_gap < back_gap);
    // All in bar range
    for (auto p : pts) CHECK(p >= 90.0 - 1e-9 && p <= 110.0 + 1e-9);
}

static void test_back_loaded_density_near_close() {
    std::printf("test_back_loaded_density_near_close\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path(bar, 10, MagnifierDistribution::BACK_LOADED);
    CHECK(pts.size() == 10);
    CHECK(near(pts[0], 100.0));
    CHECK(near(pts[9], 105.0));
    double front_gap = std::fabs(pts[1] - pts[0]);
    double back_gap  = std::fabs(pts[9] - pts[8]);
    CHECK(back_gap < front_gap);
    for (auto p : pts) CHECK(p >= 90.0 - 1e-9 && p <= 110.0 + 1e-9);
}

// ─── volume_weighted sampling ────────────────────────────────────────────────

static void test_volume_weighted_high_vol_gets_more_samples() {
    std::printf("test_volume_weighted_high_vol_gets_more_samples\n");
    Bar low_vol{100.0, 110.0, 90.0, 105.0,  500.0, 0};
    Bar hi_vol {100.0, 110.0, 90.0, 105.0, 2000.0, 0};
    auto pts_low = sample_price_path_volume_weighted(low_vol, 4, 1000.0);
    auto pts_hi  = sample_price_path_volume_weighted(hi_vol,  4, 1000.0);
    // hi_vol has 2x mean volume -> should get ~8 samples; low_vol has 0.5x
    // mean -> should get ~2 samples (clamped to min 2).
    CHECK(pts_hi.size() > pts_low.size());
    // Both must start with O and end with C.
    CHECK(near(pts_low.front(), 100.0));
    CHECK(near(pts_low.back(),  105.0));
    CHECK(near(pts_hi.front(),  100.0));
    CHECK(near(pts_hi.back(),   105.0));
}

static void test_volume_weighted_clamps_extremes() {
    std::printf("test_volume_weighted_clamps_extremes\n");
    // Ratio 10x average should be clamped to the max (4x -> 16 samples here).
    Bar absurd{100.0, 110.0, 90.0, 105.0, 10'000.0, 0};
    auto pts = sample_price_path_volume_weighted(absurd, 4, 1000.0, 2, 16);
    CHECK(pts.size() <= 16);
    CHECK(pts.size() >= 2);
}

static void test_volume_weighted_zero_ref_falls_back_to_base() {
    std::printf("test_volume_weighted_zero_ref_falls_back_to_base\n");
    Bar bar{100.0, 110.0, 90.0, 105.0, 1000.0, 0};
    auto pts = sample_price_path_volume_weighted(bar, 6, /*mean_volume=*/0.0);
    CHECK((int)pts.size() == 6);
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    test_bullish_4_endpoints();
    test_bearish_4_endpoints();
    test_doji_treated_as_bullish();
    test_bullish_open_near_high_uses_high_first_path();
    test_bearish_open_near_low_uses_low_first_path();
    test_uniform_8_samples();
    test_cosine_8_samples();
    test_min_2_samples();
    test_min_clamp_to_2();
    test_degenerate_bar();
    test_triangle_8_samples();
    test_front_loaded_density_near_open();
    test_back_loaded_density_near_close();
    test_volume_weighted_high_vol_gets_more_samples();
    test_volume_weighted_clamps_extremes();
    test_volume_weighted_zero_ref_falls_back_to_base();

    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
