#include <pineforge/ta.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
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
// Poison harness for the recompute-before-first-compute invariant.
//
// Construct an indicator into a heap buffer pre-filled with a fixed non-zero
// byte pattern. Any save-state (`saved_*`) member the constructor leaves
// UNINITIALIZED then reads as that deterministic garbage, so a recompute()
// issued before the first compute() (which calls restore() on indeterminate
// memory) misbehaves REPRODUCIBLY rather than flakily — giving the invariant
// test reliable teeth. Class-type members (deques, sub-indicators) are still
// default-constructed normally by the constructor; only uninitialized scalar
// save-state retains the poison. Once a class's ctor initializes every
// saved_* member (the fix), the poison is fully overwritten and the test
// passes under every pattern.
template <typename T, typename... Args>
static T* poison_new(unsigned char pattern, Args&&... args) {
    void* buf = ::operator new(sizeof(T));
    std::memset(buf, pattern, sizeof(T));
    if constexpr (sizeof...(Args) == 0) {
        // DEFAULT-initialization (note: no parentheses). `new (buf) T()` would
        // VALUE-initialize, which for a class with a defaulted (= default)
        // default constructor zero-initializes the whole object FIRST — wiping
        // the poison and hiding an uninitialized saved_* behind a lucky zero.
        // `new (buf) T` runs only the constructor, so members the ctor does not
        // initialize keep the poison.
        return new (buf) T;
    } else {
        return new (buf) T(std::forward<Args>(args)...);
    }
}
template <typename T>
static void poison_delete(T* p) {
    p->~T();
    ::operator delete(static_cast<void*>(p));
}

// Drive a recompute-FIRST series (bar 0 via recompute(), bars 1..K-1 via
// compute()) on a poisoned instance and compare bar-for-bar against a pure
// compute-first reference. rec(o,i)/com(o,i) call the class's recompute/compute
// with the per-bar args for bar i; both return values are coerced to double
// (bool auto-converts) for the na-aware eq() comparison.
template <typename Factory, typename Rec, typename Com>
static bool series_matches(unsigned char pat, Factory make, int K, Rec rec, Com com) {
    auto* a = make(pat);   // recompute-first (poisoned save-state on bar 0)
    auto* b = make(pat);   // reference: pure compute-first (never restores)
    bool ok = eq((double)rec(a, 0), (double)com(b, 0));
    for (int i = 1; i < K; ++i) {
        if (!eq((double)com(a, i), (double)com(b, i))) ok = false;
    }
    poison_delete(a);
    poison_delete(b);
    return ok;
}

// Run the invariant under several poison patterns. A class whose ctor leaves
// any saved_* uninitialized diverges under at least one pattern; requiring ALL
// to hold maximizes the teeth. The patterns are complementary:
//   0xFF -> every saved_* double reads as NaN, reproducing the original
//           NaN-poisoning failure mode and diverging for additive/accumulator
//           state (a tiny-magnitude garbage double would be absorbed by FP
//           addition and hide the bug);
//   0xAA -> a small finite garbage double, exercising threshold/sign paths;
//   0x00 -> zeros, which surface bugs that only appear when a garbage
//           `first_bar_`/`initialized_` byte reads FALSE (so the indicator
//           skips its first-bar branch). After the fix all patterns pass.
template <typename Factory, typename Rec, typename Com>
static bool all_patterns_match(Factory make, int K, Rec rec, Com com) {
    bool ff = series_matches(0xFF, make, K, rec, com);
    bool aa = series_matches(0xAA, make, K, rec, com);
    bool zz = series_matches(0x00, make, K, rec, com);
    return ff && aa && zz;
}

static bool eq_st(const ta::SupertrendResult& x, const ta::SupertrendResult& y) {
    return eq(x.value, y.value) && eq(x.direction, y.direction);
}
static bool eq_dmi(const ta::DMIResult& x, const ta::DMIResult& y) {
    return eq(x.diplus, y.diplus) && eq(x.diminus, y.diminus) && eq(x.adx, y.adx);
}

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
// Regression: recompute() BEFORE the first compute().
//
// In the bar-magnifier + lookahead request.security aggregation path, the
// security's first feed is a PARTIAL aggregated bar, which dispatches
// recompute() before any complete-bar compute() has run. recompute() calls
// restore(), which reads the save-state members (saved_*). If those are not
// initialized by the constructor they hold indeterminate memory, and the
// restore poisons the RMA/RSI with NaN non-deterministically — observed as a
// 0-vs-N trade-count flip across identical runs of the same binary.
//
// The fix (src/ta_moving_averages.cpp RMA::RMA, src/ta_oscillators.cpp
// RSI::RSI) initializes saved_* to mirror the initial committed state, so a
// recompute-first restores a well-defined pristine state and behaves exactly
// like compute-first. These tests lock that invariant: a recompute-first
// warmup must (a) equal the pure compute-first series bar-for-bar and (b)
// warm up to a finite (non-NaN) value rather than staying poisoned.
// ============================================================================
void test_rma_recompute_before_first_compute() {
    printf("Test RMA recompute before first compute... ");
    const int len = 5;
    ta::RMA a(len);   // first bar arrives via recompute (partial sub-bar)
    ta::RMA b(len);   // reference: pure compute path
    CHECK_EQ(a.recompute(prices[0]), b.compute(prices[0]),
             "RMA recompute-first first bar == compute-first first bar");
    double ra = na<double>(), rb = na<double>();
    for (int i = 1; i < len + 3; i++) {
        ra = a.compute(prices[i]);
        rb = b.compute(prices[i]);
        CHECK_EQ(ra, rb, "RMA committed series equal after recompute-first start");
    }
    CHECK(!is_na(ra), "RMA warms up to a finite value (no NaN poisoning)");
    printf("OK\n");
}

void test_rsi_recompute_before_first_compute() {
    printf("Test RSI recompute before first compute... ");
    const int len = 5;
    ta::RSI a(len);   // first bar arrives via recompute (partial sub-bar)
    ta::RSI b(len);   // reference: pure compute path
    CHECK_EQ(a.recompute(prices[0]), b.compute(prices[0]),
             "RSI recompute-first first bar == compute-first first bar");
    double ra = na<double>(), rb = na<double>();
    for (int i = 1; i < len + 5; i++) {
        ra = a.compute(prices[i]);
        rb = b.compute(prices[i]);
        CHECK_EQ(ra, rb, "RSI committed series equal after recompute-first start");
    }
    CHECK(!is_na(ra), "RSI warms up to a finite value (no NaN poisoning)");
    printf("OK\n");
}

// ============================================================================
// Class-wide invariant: recompute() BEFORE the first compute() must equal
// compute() on a fresh instance, for EVERY indicator with a save/restore
// (saved_*) mechanism. recompute() means "compute this bar as if the previous
// save-state were the committed state"; on a pristine object the previous
// committed state IS the constructor's initial state, so recompute-first must
// equal compute-first bar 0, and the committed series must stay identical
// thereafter. Each subtest poisons the save-state (see poison_new) so an
// uninitialized saved_* member fails DETERMINISTICALLY on the pre-fix tree.
//
// Classes whose save-state is entirely buffer/deque-based (Highest, SMA, WMA,
// StdDev, …) guard recompute() with `if (buffer.empty()) return compute(src)`,
// so they route to compute() automatically before the first bar and carry no
// scalar save-state to leak — they are covered by the existing recompute
// tests and need no poison check here. KC and ValueWhen already initialize
// their save-state (in-class initializer / empty-deque), and are safe.
// ============================================================================
void test_all_classes_recompute_before_first_compute() {
    printf("Test class-wide recompute-before-first-compute invariant...\n");

    // Shared OHLCV-like fixtures (8 bars).
    static const double H[] = {101.0, 103.0, 102.0, 105.0, 104.0, 107.0, 106.0, 109.0};
    static const double L[] = { 99.0, 100.0,  98.0, 101.0, 100.0, 103.0, 102.0, 105.0};
    static const double C[] = {100.0, 102.0,  99.0, 104.0, 101.0, 106.0, 103.0, 108.0};
    static const double Vl[] = {1000.0, 1500.0, 800.0, 2000.0, 1200.0, 1700.0, 900.0, 1300.0};
    static const int64_t TS[] = {
        1700000000000LL, 1700000060000LL, 1700000120000LL, 1700000180000LL,
        1700000240000LL, 1700000300000LL, 1700000360000LL, 1700000420000LL};

    // --- EMA ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::EMA>(p, 5); }, 8,
              [](ta::EMA* o, int i){ return o->recompute(prices[i]); },
              [](ta::EMA* o, int i){ return o->compute(prices[i]); }),
          "EMA recompute-before-first-compute == compute-first");

    // --- Crossover (bar 0: a>b so the prev-tie guard decides the result) ---
    {
        static const double A[] = {2.0, 1.0, 3.0, 1.5, 4.0, 0.5, 5.0, 2.0};
        static const double B[] = {1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
        CHECK(all_patterns_match(
                  [](unsigned char p){ return poison_new<ta::Crossover>(p); }, 8,
                  [&](ta::Crossover* o, int i){ return o->recompute(A[i], B[i]); },
                  [&](ta::Crossover* o, int i){ return o->compute(A[i], B[i]); }),
              "Crossover recompute-before-first-compute == compute-first");
    }

    // --- Crossunder (bar 0: a<b) ---
    {
        static const double A[] = {1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0};
        static const double B[] = {2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
        CHECK(all_patterns_match(
                  [](unsigned char p){ return poison_new<ta::Crossunder>(p); }, 8,
                  [&](ta::Crossunder* o, int i){ return o->recompute(A[i], B[i]); },
                  [&](ta::Crossunder* o, int i){ return o->compute(A[i], B[i]); }),
              "Crossunder recompute-before-first-compute == compute-first");
    }

    // --- Cross (bar 0: a!=b so the remembered-sign guard decides) ---
    {
        static const double A[] = {2.0, 1.0, 3.0, 1.0, 3.0, 1.0, 3.0, 1.0};
        static const double B[] = {1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
        CHECK(all_patterns_match(
                  [](unsigned char p){ return poison_new<ta::Cross>(p); }, 8,
                  [&](ta::Cross* o, int i){ return o->recompute(A[i], B[i]); },
                  [&](ta::Cross* o, int i){ return o->compute(A[i], B[i]); }),
              "Cross recompute-before-first-compute == compute-first");
    }

    // --- ATR ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::ATR>(p, 3); }, 8,
              [&](ta::ATR* o, int i){ return o->recompute(H[i], L[i], C[i]); },
              [&](ta::ATR* o, int i){ return o->compute(H[i], L[i], C[i]); }),
          "ATR recompute-before-first-compute == compute-first");

    // --- TR ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::TR>(p, false); }, 8,
              [&](ta::TR* o, int i){ return o->recompute(H[i], L[i], C[i]); },
              [&](ta::TR* o, int i){ return o->compute(H[i], L[i], C[i]); }),
          "TR recompute-before-first-compute == compute-first");

    // --- MFI ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::MFI>(p, 3); }, 8,
              [&](ta::MFI* o, int i){ return o->recompute(C[i], Vl[i]); },
              [&](ta::MFI* o, int i){ return o->compute(C[i], Vl[i]); }),
          "MFI recompute-before-first-compute == compute-first");

    // --- CMO ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::CMO>(p, 3); }, 8,
              [](ta::CMO* o, int i){ return o->recompute(prices[i]); },
              [](ta::CMO* o, int i){ return o->compute(prices[i]); }),
          "CMO recompute-before-first-compute == compute-first");

    // --- TSI ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::TSI>(p, 3, 5); }, 8,
              [](ta::TSI* o, int i){ return o->recompute(prices[i]); },
              [](ta::TSI* o, int i){ return o->compute(prices[i]); }),
          "TSI recompute-before-first-compute == compute-first");

    // --- Cum ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::Cum>(p); }, 8,
              [](ta::Cum* o, int i){ return o->recompute(prices[i]); },
              [](ta::Cum* o, int i){ return o->compute(prices[i]); }),
          "Cum recompute-before-first-compute == compute-first");

    // --- AllTimeMax (bar 0 very negative so a poisoned has_=true/max_~0 leaks) ---
    {
        static const double X[] = {-1.0e6, 101.5, 99.8, 102.3, 98.5, 103.0, 101.2, 104.5};
        CHECK(all_patterns_match(
                  [](unsigned char p){ return poison_new<ta::AllTimeMax>(p); }, 8,
                  [&](ta::AllTimeMax* o, int i){ return o->recompute(X[i]); },
                  [&](ta::AllTimeMax* o, int i){ return o->compute(X[i]); }),
              "AllTimeMax recompute-before-first-compute == compute-first");
    }

    // --- AllTimeMin (bar 0 very positive) ---
    {
        static const double X[] = {1.0e6, 101.5, 99.8, 102.3, 98.5, 103.0, 101.2, 104.5};
        CHECK(all_patterns_match(
                  [](unsigned char p){ return poison_new<ta::AllTimeMin>(p); }, 8,
                  [&](ta::AllTimeMin* o, int i){ return o->recompute(X[i]); },
                  [&](ta::AllTimeMin* o, int i){ return o->compute(X[i]); }),
              "AllTimeMin recompute-before-first-compute == compute-first");
    }

    // --- BarsSince (bar 0 condition=false so a poisoned ever_true_ leaks) ---
    {
        static const bool COND[] = {false, false, true, false, false, true, false, false};
        CHECK(all_patterns_match(
                  [](unsigned char p){ return poison_new<ta::BarsSince>(p); }, 8,
                  [&](ta::BarsSince* o, int i){ return o->recompute(COND[i]); },
                  [&](ta::BarsSince* o, int i){ return o->compute(COND[i]); }),
              "BarsSince recompute-before-first-compute == compute-first");
    }

    // --- SAR ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::SAR>(p, 0.02, 0.02, 0.2); }, 8,
              [&](ta::SAR* o, int i){ return o->recompute(H[i], L[i], C[i]); },
              [&](ta::SAR* o, int i){ return o->compute(H[i], L[i], C[i]); }),
          "SAR recompute-before-first-compute == compute-first");

    // --- OBV ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::OBV>(p); }, 8,
              [&](ta::OBV* o, int i){ return o->recompute(C[i], Vl[i]); },
              [&](ta::OBV* o, int i){ return o->compute(C[i], Vl[i]); }),
          "OBV recompute-before-first-compute == compute-first");

    // --- AccDist ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::AccDist>(p); }, 8,
              [&](ta::AccDist* o, int i){ return o->recompute(H[i], L[i], C[i], Vl[i]); },
              [&](ta::AccDist* o, int i){ return o->compute(H[i], L[i], C[i], Vl[i]); }),
          "AccDist recompute-before-first-compute == compute-first");

    // --- NVI ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::NVI>(p); }, 8,
              [&](ta::NVI* o, int i){ return o->recompute(C[i], Vl[i]); },
              [&](ta::NVI* o, int i){ return o->compute(C[i], Vl[i]); }),
          "NVI recompute-before-first-compute == compute-first");

    // --- PVI ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::PVI>(p); }, 8,
              [&](ta::PVI* o, int i){ return o->recompute(C[i], Vl[i]); },
              [&](ta::PVI* o, int i){ return o->compute(C[i], Vl[i]); }),
          "PVI recompute-before-first-compute == compute-first");

    // --- PVT ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::PVT>(p); }, 8,
              [&](ta::PVT* o, int i){ return o->recompute(C[i], Vl[i]); },
              [&](ta::PVT* o, int i){ return o->compute(C[i], Vl[i]); }),
          "PVT recompute-before-first-compute == compute-first");

    // --- WAD ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::WAD>(p); }, 8,
              [&](ta::WAD* o, int i){ return o->recompute(H[i], L[i], C[i]); },
              [&](ta::WAD* o, int i){ return o->compute(H[i], L[i], C[i]); }),
          "WAD recompute-before-first-compute == compute-first");

    // --- VWAP ---
    CHECK(all_patterns_match(
              [](unsigned char p){ return poison_new<ta::VWAP>(p); }, 8,
              [&](ta::VWAP* o, int i){ return o->recompute(C[i], Vl[i], TS[i]); },
              [&](ta::VWAP* o, int i){ return o->compute(C[i], Vl[i], TS[i]); }),
          "VWAP recompute-before-first-compute == compute-first");

    // --- Supertrend (struct result: compare value + direction) ---
    {
        auto run = [&](unsigned char pat) {
            auto* a = poison_new<ta::Supertrend>(pat, 3.0, 2);
            auto* b = poison_new<ta::Supertrend>(pat, 3.0, 2);
            bool ok = eq_st(a->recompute(H[0], L[0], C[0]), b->compute(H[0], L[0], C[0]));
            for (int i = 1; i < 8; ++i)
                if (!eq_st(a->compute(H[i], L[i], C[i]), b->compute(H[i], L[i], C[i]))) ok = false;
            poison_delete(a); poison_delete(b);
            return ok;
        };
        CHECK(run(0xFF) && run(0xAA) && run(0x00),
              "Supertrend recompute-before-first-compute == compute-first");
    }

    // --- DMI (struct result: compare diplus/diminus/adx) ---
    {
        auto run = [&](unsigned char pat) {
            auto* a = poison_new<ta::DMI>(pat, 2, 2);
            auto* b = poison_new<ta::DMI>(pat, 2, 2);
            bool ok = eq_dmi(a->recompute(H[0], L[0], C[0]), b->compute(H[0], L[0], C[0]));
            for (int i = 1; i < 8; ++i)
                if (!eq_dmi(a->compute(H[i], L[i], C[i]), b->compute(H[i], L[i], C[i]))) ok = false;
            poison_delete(a); poison_delete(b);
            return ok;
        };
        CHECK(run(0xFF) && run(0xAA) && run(0x00),
              "DMI recompute-before-first-compute == compute-first");
    }

    printf("  ... done\n");
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
// Runtime semantics validated against TradingView's `ta.pivothigh` /
// `ta.pivotlow`: equal on left allowed, equal on right invalidates. A
// flat-top run reports the pivot exactly one bar AFTER the last flat bar
// (TV's pivot detection is right-exclusive on ties so a streak in progress
// can still be broken by a higher right-side bar).
// ============================================================================

void test_pivothigh_equal_right_invalidates() {
    printf("Test PivotHigh tie on right is not a pivot... ");
    ta::PivotHigh ph(1, 1);
    ph.compute(1.0);
    ph.compute(2.0);
    CHECK_EQ(ph.compute(2.0), na<double>(), "equal high on right => na");
    printf("OK\n");
}

void test_pivothigh_equal_left_allowed() {
    printf("Test PivotHigh tie on left is allowed... ");
    ta::PivotHigh ph(1, 1);
    ph.compute(2.0);
    ph.compute(2.0);
    CHECK_EQ(ph.compute(1.0), 2.0, "equal high on left => pivot at right of flat-top");
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

void test_pivotlow_equal_right_invalidates() {
    printf("Test PivotLow tie on right is not a pivot... ");
    ta::PivotLow pl(1, 1);
    pl.compute(3.0);
    pl.compute(2.0);
    CHECK_EQ(pl.compute(2.0), na<double>(), "equal low on right => na");
    printf("OK\n");
}

void test_pivotlow_equal_left_allowed() {
    printf("Test PivotLow tie on left is allowed... ");
    ta::PivotLow pl(1, 1);
    pl.compute(2.0);
    pl.compute(2.0);
    CHECK_EQ(pl.compute(3.0), 2.0, "equal low on left => pivot at right of flat-bottom");
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
    if (is_na(a) && is_na(b)) return true;
    if (is_na(a) || is_na(b)) return false;
    return std::abs(a - b) < tol;
}

void test_pivot_traditional() {
    printf("Test pivot_point_levels Traditional... ");
    auto levels = ta::pivot_point_levels("Traditional", 110, 90, 100);
    CHECK(levels.size() == 11, "Traditional should return 11 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[0], p), "Traditional pivot value");
    double S1 = 2.0 * p - 110.0;
    double R1 = 2.0 * p - 90.0;
    CHECK(near(levels[2], S1), "Traditional S1");
    CHECK(near(levels[1], R1), "Traditional R1");
    CHECK(near(levels[9], p + 4.0 * 20.0), "Traditional R5");
    CHECK(near(levels[10], p - 4.0 * 20.0), "Traditional S5");
    printf("OK\n");
}

void test_pivot_fibonacci() {
    printf("Test pivot_point_levels Fibonacci... ");
    auto levels = ta::pivot_point_levels("Fibonacci", 110, 90, 100);
    CHECK(levels.size() == 11, "Fibonacci should return 11 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[0], p), "Fibonacci pivot value");
    double range = 20.0;
    CHECK(near(levels[2], p - 0.382 * range), "Fibonacci S1");
    CHECK(near(levels[1], p + 0.382 * range), "Fibonacci R1");
    printf("OK\n");
}

void test_pivot_woodie() {
    printf("Test pivot_point_levels Woodie... ");
    auto levels = ta::pivot_point_levels("Woodie", 110, 90, 100);
    CHECK(levels.size() == 11, "Woodie should return 11 levels");
    double p = (110.0 + 90.0 + 200.0) / 4.0;
    CHECK(near(levels[0], p), "Woodie pivot value");
    printf("OK\n");
}

void test_pivot_classic() {
    printf("Test pivot_point_levels Classic... ");
    auto levels = ta::pivot_point_levels("Classic", 110, 90, 100);
    CHECK(levels.size() == 11, "Classic should return 11 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[0], p), "Classic pivot value");
    CHECK(near(levels[7], p + 3.0 * 20.0), "Classic R4");
    CHECK(near(levels[8], p - 3.0 * 20.0), "Classic S4");
    printf("OK\n");
}

void test_pivot_dm() {
    printf("Test pivot_point_levels DM... ");
    // close != high and close != low => x = H + L + 2C
    auto levels = ta::pivot_point_levels("DM", 110, 90, 100);
    CHECK(levels.size() == 11, "DM should return 11 levels");
    double x = 110.0 + 90.0 + 200.0;
    double p = x / 4.0;
    CHECK(near(levels[0], p), "DM pivot value");
    CHECK(near(levels[2], x / 2.0 - 110.0), "DM S1");
    CHECK(near(levels[1], x / 2.0 - 90.0), "DM R1");
    printf("OK\n");
}

void test_pivot_camarilla() {
    printf("Test pivot_point_levels Camarilla... ");
    auto levels = ta::pivot_point_levels("Camarilla", 110, 90, 100);
    CHECK(levels.size() == 11, "Camarilla should return 11 levels");
    double p = (110.0 + 90.0 + 100.0) / 3.0;
    CHECK(near(levels[0], p), "Camarilla pivot value");
    double r = 20.0;
    CHECK(near(levels[2], 100.0 - r * 1.1 / 12.0), "Camarilla S1");
    CHECK(near(levels[1], 100.0 + r * 1.1 / 12.0), "Camarilla R1");
    CHECK(near(levels[9], (110.0 / 90.0) * 100.0), "Camarilla R5");
    printf("OK\n");
}

void test_pivot_unknown() {
    printf("Test pivot_point_levels unknown method... ");
    auto levels = ta::pivot_point_levels("SomethingElse", 110, 90, 100);
    CHECK(levels.size() == 11, "Unknown method should return 11 levels");
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
    test_rma_recompute_before_first_compute();
    test_rsi_recompute_before_first_compute();
    test_all_classes_recompute_before_first_compute();
    test_highest_lowest_recompute();
    test_stddev_recompute();
    test_macd_recompute();
    test_bb_recompute();
    test_crossover_recompute();
    test_multiple_recomputes();
    test_ema_multiple_recomputes();
    test_compute_after_recompute();

    printf("\n=== PivotHigh / PivotLow tests ===\n\n");
    test_pivothigh_equal_right_invalidates();
    test_pivothigh_equal_left_allowed();
    test_pivothigh_strict_peak();
    test_pivotlow_equal_right_invalidates();
    test_pivotlow_equal_left_allowed();
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
