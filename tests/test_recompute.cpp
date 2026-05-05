#include <pineforge/ta.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace pineforge;

static constexpr double EPS = 1e-10;

static bool eq(double a, double b) {
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::abs(a - b) < EPS;
}

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
    double _a = (a), _b = (b); \
    if (!eq(_a, _b)) { \
        printf("FAIL: %s (line %d): %.15f != %.15f\n", msg, __LINE__, _a, _b); \
        tests_failed++; \
    } else { \
        tests_passed++; \
    } \
} while(0)

// Test data - some realistic price-like values
static const std::vector<double> prices = {
    100.0, 101.5, 99.8, 102.3, 98.5, 103.0, 101.2, 104.5, 99.0, 105.0,
    102.0, 106.5, 97.5, 103.8, 100.5, 107.0, 98.0, 104.2, 101.8, 108.0
};

// ============================================================================
// Test 1: SMA recompute
// ============================================================================
void test_sma_recompute() {
    printf("Test SMA recompute... ");
    const int len = 5;
    const int N = 10;
    double val_A = 999.0;
    double val_B = 50.0;

    // Instance 1: compute N-1 bars, then compute(A), then recompute(B)
    ta::SMA sma1(len);
    for (int i = 0; i < N - 1; i++) sma1.compute(prices[i]);
    sma1.compute(val_A);
    double result1 = sma1.recompute(val_B);

    // Instance 2: compute N-1 bars, then compute(B)
    ta::SMA sma2(len);
    for (int i = 0; i < N - 1; i++) sma2.compute(prices[i]);
    double result2 = sma2.compute(val_B);

    CHECK_EQ(result1, result2, "SMA recompute should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test 2: EMA recompute
// ============================================================================
void test_ema_recompute() {
    printf("Test EMA recompute... ");
    const int len = 5;
    const int N = 10;
    double val_A = 999.0;
    double val_B = 50.0;

    ta::EMA ema1(len);
    for (int i = 0; i < N - 1; i++) ema1.compute(prices[i]);
    ema1.compute(val_A);
    double result1 = ema1.recompute(val_B);

    ta::EMA ema2(len);
    for (int i = 0; i < N - 1; i++) ema2.compute(prices[i]);
    double result2 = ema2.compute(val_B);

    CHECK_EQ(result1, result2, "EMA recompute should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test 3: RMA recompute
// ============================================================================
void test_rma_recompute() {
    printf("Test RMA recompute... ");
    const int len = 5;
    const int N = 10;
    double val_A = 999.0;
    double val_B = 50.0;

    ta::RMA rma1(len);
    for (int i = 0; i < N - 1; i++) rma1.compute(prices[i]);
    rma1.compute(val_A);
    double result1 = rma1.recompute(val_B);

    ta::RMA rma2(len);
    for (int i = 0; i < N - 1; i++) rma2.compute(prices[i]);
    double result2 = rma2.compute(val_B);

    CHECK_EQ(result1, result2, "RMA recompute should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test 4: RSI recompute
// ============================================================================
void test_rsi_recompute() {
    printf("Test RSI recompute... ");
    const int len = 5;
    const int N = 12;
    double val_A = 999.0;
    double val_B = 50.0;

    ta::RSI rsi1(len);
    for (int i = 0; i < N - 1; i++) rsi1.compute(prices[i]);
    rsi1.compute(val_A);
    double result1 = rsi1.recompute(val_B);

    ta::RSI rsi2(len);
    for (int i = 0; i < N - 1; i++) rsi2.compute(prices[i]);
    double result2 = rsi2.compute(val_B);

    CHECK_EQ(result1, result2, "RSI recompute should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test 5: Highest / Lowest recompute
// ============================================================================
void test_highest_lowest_recompute() {
    printf("Test Highest/Lowest recompute... ");
    const int len = 5;
    const int N = 10;
    double val_A = 999.0;
    double val_B = 50.0;

    // Highest
    ta::Highest hi1(len), hi2(len);
    for (int i = 0; i < N - 1; i++) { hi1.compute(prices[i]); hi2.compute(prices[i]); }
    hi1.compute(val_A);
    double r1 = hi1.recompute(val_B);
    double r2 = hi2.compute(val_B);
    CHECK_EQ(r1, r2, "Highest recompute should equal fresh compute");

    // Lowest
    ta::Lowest lo1(len), lo2(len);
    for (int i = 0; i < N - 1; i++) { lo1.compute(prices[i]); lo2.compute(prices[i]); }
    lo1.compute(val_A);
    r1 = lo1.recompute(val_B);
    r2 = lo2.compute(val_B);
    CHECK_EQ(r1, r2, "Lowest recompute should equal fresh compute");

    printf("OK\n");
}

// ============================================================================
// Test 6: StdDev recompute
// ============================================================================
void test_stddev_recompute() {
    printf("Test StdDev recompute... ");
    const int len = 5;
    const int N = 10;
    double val_A = 999.0;
    double val_B = 50.0;

    ta::StdDev sd1(len), sd2(len);
    for (int i = 0; i < N - 1; i++) { sd1.compute(prices[i]); sd2.compute(prices[i]); }
    sd1.compute(val_A);
    double r1 = sd1.recompute(val_B);
    double r2 = sd2.compute(val_B);
    CHECK_EQ(r1, r2, "StdDev recompute should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test 7: MACD recompute
// ============================================================================
void test_macd_recompute() {
    printf("Test MACD recompute... ");
    const int N = 18;
    double val_A = 999.0;
    double val_B = 50.0;

    ta::MACD macd1(3, 5, 3), macd2(3, 5, 3);
    for (int i = 0; i < N - 1; i++) { macd1.compute(prices[i]); macd2.compute(prices[i]); }
    macd1.compute(val_A);
    auto r1 = macd1.recompute(val_B);
    auto r2 = macd2.compute(val_B);
    CHECK_EQ(r1.macd_line, r2.macd_line, "MACD macd_line recompute");
    CHECK_EQ(r1.signal_line, r2.signal_line, "MACD signal_line recompute");
    CHECK_EQ(r1.histogram, r2.histogram, "MACD histogram recompute");
    printf("OK\n");
}

// ============================================================================
// Test 8: BB recompute
// ============================================================================
void test_bb_recompute() {
    printf("Test BB recompute... ");
    const int len = 5;
    const int N = 10;
    double val_A = 999.0;
    double val_B = 50.0;

    ta::BB bb1(len, 2.0), bb2(len, 2.0);
    for (int i = 0; i < N - 1; i++) { bb1.compute(prices[i]); bb2.compute(prices[i]); }
    bb1.compute(val_A);
    auto r1 = bb1.recompute(val_B);
    auto r2 = bb2.compute(val_B);
    CHECK_EQ(r1.middle, r2.middle, "BB middle recompute");
    CHECK_EQ(r1.upper, r2.upper, "BB upper recompute");
    CHECK_EQ(r1.lower, r2.lower, "BB lower recompute");
    printf("OK\n");
}

// ============================================================================
// Test 9: Crossover recompute
// ============================================================================
void test_crossover_recompute() {
    printf("Test Crossover recompute... ");

    ta::Crossover co1, co2;
    // Feed same initial data
    co1.compute(1.0, 2.0); co2.compute(1.0, 2.0);  // a < b
    co1.compute(3.0, 2.0); // a > b, crossover! But we'll recompute
    bool r1 = co1.recompute(1.5, 2.0); // a < b, no crossover
    bool r2 = co2.compute(1.5, 2.0);
    CHECK(r1 == r2, "Crossover recompute no-cross case");

    // Now test where recompute produces a crossover
    ta::Crossover co3, co4;
    co3.compute(1.0, 2.0); co4.compute(1.0, 2.0);  // a < b (prev)
    co3.compute(1.5, 2.0); // no crossover, but recompute with crossover
    r1 = co3.recompute(3.0, 2.0); // a > b with prev a < b -> crossover
    r2 = co4.compute(3.0, 2.0);
    CHECK(r1 == r2, "Crossover recompute cross case");
    CHECK(r1 == true, "Crossover recompute should detect crossing");

    printf("OK\n");
}

// ============================================================================
// Test 10: Multiple recomputes in sequence (SMA)
// ============================================================================
void test_multiple_recomputes() {
    printf("Test multiple recomputes... ");
    const int len = 5;
    const int N = 10;

    ta::SMA sma1(len);
    for (int i = 0; i < N - 1; i++) sma1.compute(prices[i]);
    sma1.compute(100.0);        // initial last bar
    sma1.recompute(200.0);      // replace with 200
    sma1.recompute(300.0);      // replace with 300
    double r1 = sma1.recompute(50.0); // replace with 50

    ta::SMA sma2(len);
    for (int i = 0; i < N - 1; i++) sma2.compute(prices[i]);
    double r2 = sma2.compute(50.0);

    CHECK_EQ(r1, r2, "Multiple SMA recomputes should equal fresh compute");

    // Also verify intermediate recomputes work
    ta::SMA sma3(len);
    for (int i = 0; i < N - 1; i++) sma3.compute(prices[i]);
    sma3.compute(100.0);
    double r3 = sma3.recompute(200.0);

    ta::SMA sma4(len);
    for (int i = 0; i < N - 1; i++) sma4.compute(prices[i]);
    double r4 = sma4.compute(200.0);
    CHECK_EQ(r3, r4, "Intermediate recompute should equal fresh compute");

    printf("OK\n");
}

// ============================================================================
// Test 11: EMA multiple recomputes
// ============================================================================
void test_ema_multiple_recomputes() {
    printf("Test EMA multiple recomputes... ");
    const int len = 5;
    const int N = 10;

    ta::EMA ema1(len);
    for (int i = 0; i < N - 1; i++) ema1.compute(prices[i]);
    ema1.compute(100.0);
    ema1.recompute(200.0);
    ema1.recompute(300.0);
    double r1 = ema1.recompute(50.0);

    ta::EMA ema2(len);
    for (int i = 0; i < N - 1; i++) ema2.compute(prices[i]);
    double r2 = ema2.compute(50.0);

    CHECK_EQ(r1, r2, "Multiple EMA recomputes should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test 12: Verify compute still works correctly after recompute
// ============================================================================
void test_compute_after_recompute() {
    printf("Test compute after recompute... ");
    const int len = 5;
    const int N = 8;

    ta::SMA sma1(len);
    for (int i = 0; i < N; i++) sma1.compute(prices[i]);
    sma1.recompute(50.0);        // replace bar N with 50
    double r1 = sma1.compute(75.0); // now add bar N+1

    ta::SMA sma2(len);
    for (int i = 0; i < N - 1; i++) sma2.compute(prices[i]);
    sma2.compute(50.0);
    double r2 = sma2.compute(75.0);

    CHECK_EQ(r1, r2, "compute() after recompute() should work correctly");
    printf("OK\n");
}

// ============================================================================
// PivotHigh / PivotLow parity + recompute == compute.
// Current runtime semantics: strict on left side, equal-on-right accepted.
// ============================================================================

void test_pivothigh_equal_right_allowed() {
    printf("Test PivotHigh tie on right is allowed... ");
    ta::PivotHigh ph(1, 1);
    ph.compute(1.0);
    ph.compute(2.0);
    CHECK_EQ(ph.compute(2.0), 2.0, "equal high on right => pivot");
    printf("OK\n");
}

void test_pivothigh_equal_left_invalidates() {
    printf("Test PivotHigh tie on left is not a pivot... ");
    ta::PivotHigh ph(1, 1);
    ph.compute(2.0);
    ph.compute(2.0);
    CHECK_EQ(ph.compute(1.0), na<double>(), "equal high on left => na");
    printf("OK\n");
}

void test_pivothigh_strict_peak() {
    printf("Test PivotHigh strict peak... ");
    ta::PivotHigh ph(1, 1);
    ph.compute(1.0);
    ph.compute(3.0);
    CHECK_EQ(ph.compute(1.0), 3.0, "isolated high");
    printf("OK\n");
}

void test_pivotlow_equal_right_allowed() {
    printf("Test PivotLow tie on right is allowed... ");
    ta::PivotLow pl(1, 1);
    pl.compute(3.0);
    pl.compute(2.0);
    CHECK_EQ(pl.compute(2.0), 2.0, "equal low on right => pivot");
    printf("OK\n");
}

void test_pivotlow_equal_left_invalidates() {
    printf("Test PivotLow tie on left is not a pivot... ");
    ta::PivotLow pl(1, 1);
    pl.compute(2.0);
    pl.compute(2.0);
    CHECK_EQ(pl.compute(3.0), na<double>(), "equal low on left => na");
    printf("OK\n");
}

void test_pivotlow_strict_trough() {
    printf("Test PivotLow strict trough... ");
    ta::PivotLow pl(1, 1);
    pl.compute(3.0);
    pl.compute(1.0);
    CHECK_EQ(pl.compute(3.0), 1.0, "isolated low");
    printf("OK\n");
}

/// First non-na after (left + right + 1) samples — pivot confirms once the full right leg exists.
void test_pivothigh_confirmation_requires_full_window() {
    printf("Test PivotHigh confirmation window (left=1, right=2)... ");
    ta::PivotHigh ph(1, 2);
    CHECK_EQ(ph.compute(5.0), na<double>(), "bar 1");
    CHECK_EQ(ph.compute(10.0), na<double>(), "bar 2");
    CHECK_EQ(ph.compute(6.0), na<double>(), "bar 3");
    CHECK_EQ(ph.compute(7.0), 10.0, "bar 4 — window [5,10,6,7], pivot at 10");
    printf("OK\n");
}

void test_pivothigh_recompute_matches_compute() {
    printf("Test PivotHigh recompute equals fresh compute... ");
    ta::PivotHigh ph1(1, 1);
    ta::PivotHigh ph2(1, 1);
    ph1.compute(1.0);
    ph1.compute(3.0);
    ph1.compute(1.0);
    double r1 = ph1.recompute(2.0);

    ph2.compute(1.0);
    ph2.compute(3.0);
    double r2 = ph2.compute(2.0);

    CHECK_EQ(r1, r2, "PivotHigh recompute should equal fresh compute");
    printf("OK\n");
}

void test_pivotlow_recompute_matches_compute() {
    printf("Test PivotLow recompute equals fresh compute... ");
    ta::PivotLow pl1(1, 1);
    ta::PivotLow pl2(1, 1);
    pl1.compute(3.0);
    pl1.compute(1.0);
    pl1.compute(3.0);
    double r1 = pl1.recompute(2.0);

    pl2.compute(3.0);
    pl2.compute(1.0);
    double r2 = pl2.compute(2.0);

    CHECK_EQ(r1, r2, "PivotLow recompute should equal fresh compute");
    printf("OK\n");
}

// ============================================================================
// Test: pivot_point_levels
// ============================================================================

static bool near(double a, double b, double tol = 1e-9) {
    return std::abs(a - b) < tol;
}

void test_pivot_traditional() {
    printf("Test pivot_point_levels Traditional... ");
    auto levels = ta::pivot_point_levels("Traditional", 110, 90, 100);
    CHECK(levels.size() == 7, "Traditional should return 7 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[3], p), "Traditional pivot value");
    double S1 = 2.0 * p - 110.0;
    double R1 = 2.0 * p - 90.0;
    CHECK(near(levels[2], S1), "Traditional S1");
    CHECK(near(levels[4], R1), "Traditional R1");
    printf("OK\n");
}

void test_pivot_fibonacci() {
    printf("Test pivot_point_levels Fibonacci... ");
    auto levels = ta::pivot_point_levels("Fibonacci", 110, 90, 100);
    CHECK(levels.size() == 7, "Fibonacci should return 7 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[3], p), "Fibonacci pivot value");
    double range = 20.0;
    CHECK(near(levels[2], p - 0.382 * range), "Fibonacci S1");
    CHECK(near(levels[4], p + 0.382 * range), "Fibonacci R1");
    printf("OK\n");
}

void test_pivot_woodie() {
    printf("Test pivot_point_levels Woodie... ");
    auto levels = ta::pivot_point_levels("Woodie", 110, 90, 100);
    CHECK(levels.size() == 5, "Woodie should return 5 levels");
    double p = (110.0 + 90.0 + 200.0) / 4.0;
    CHECK(near(levels[2], p), "Woodie pivot value");
    printf("OK\n");
}

void test_pivot_classic() {
    printf("Test pivot_point_levels Classic... ");
    auto levels = ta::pivot_point_levels("Classic", 110, 90, 100);
    CHECK(levels.size() == 5, "Classic should return 5 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[2], p), "Classic pivot value");
    printf("OK\n");
}

void test_pivot_dm() {
    printf("Test pivot_point_levels DM... ");
    // close != high and close != low => x = H + L + 2C
    auto levels = ta::pivot_point_levels("DM", 110, 90, 100);
    CHECK(levels.size() == 3, "DM should return 3 levels");
    double x = 110.0 + 90.0 + 200.0;
    double p = x / 4.0;
    CHECK(near(levels[1], p), "DM pivot value");
    CHECK(near(levels[0], x / 2.0 - 110.0), "DM S1");
    CHECK(near(levels[2], x / 2.0 - 90.0), "DM R1");
    printf("OK\n");
}

void test_pivot_camarilla() {
    printf("Test pivot_point_levels Camarilla... ");
    auto levels = ta::pivot_point_levels("Camarilla", 110, 90, 100);
    CHECK(levels.size() == 9, "Camarilla should return 9 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[4], p), "Camarilla pivot value");
    double r = 20.0;
    CHECK(near(levels[3], 100.0 - r * 1.1 / 12.0), "Camarilla S1");
    CHECK(near(levels[5], 100.0 + r * 1.1 / 12.0), "Camarilla R1");
    printf("OK\n");
}

void test_pivot_unknown() {
    printf("Test pivot_point_levels unknown method... ");
    auto levels = ta::pivot_point_levels("SomethingElse", 110, 90, 100);
    CHECK(levels.size() == 1, "Unknown method should return 1 level");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[0], p), "Unknown method pivot value");
    printf("OK\n");
}

int main() {
    printf("=== TA recompute() tests ===\n\n");

    test_sma_recompute();
    test_ema_recompute();
    test_rma_recompute();
    test_rsi_recompute();
    test_highest_lowest_recompute();
    test_stddev_recompute();
    test_macd_recompute();
    test_bb_recompute();
    test_crossover_recompute();
    test_multiple_recomputes();
    test_ema_multiple_recomputes();
    test_compute_after_recompute();

    printf("\n=== PivotHigh / PivotLow tests ===\n\n");
    test_pivothigh_equal_right_allowed();
    test_pivothigh_equal_left_invalidates();
    test_pivothigh_strict_peak();
    test_pivotlow_equal_right_allowed();
    test_pivotlow_equal_left_invalidates();
    test_pivotlow_strict_trough();
    test_pivothigh_confirmation_requires_full_window();
    test_pivothigh_recompute_matches_compute();
    test_pivotlow_recompute_matches_compute();

    printf("\n=== pivot_point_levels tests ===\n\n");
    test_pivot_traditional();
    test_pivot_fibonacci();
    test_pivot_woodie();
    test_pivot_classic();
    test_pivot_dm();
    test_pivot_camarilla();
    test_pivot_unknown();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
