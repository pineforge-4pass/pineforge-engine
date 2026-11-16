// test_ta_sma_exact_compensation -- the compensation a window sum carries is
// the exact rounding error of the addition that made it, also when a larger
// source enters a smaller sum.
//
// TradingView rows. The pulse capture of 2026-11-16 (the neighbouring SMA
// tests cite it as CAP-731-PULSE): a strategy on a 15-minute chart, 44 closed
// trades and no open one, whose entry comments carry ta.sma(x, 3) of bars
// 2..45 of its pulse window.
//     script         strategy.pine  sha256 276834fe7b2c2f7c18888e7bf69469fe6aa8bcfe85a7961e0b2d419543b26443
//     trade list     tv_trades.csv  sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28
//     decoded rows   decoded.tsv    sha256 a149d349a4302b18eaef95be2ec7cf9d6327a1d3dd108a1fdde29f2f2a554cf5
//     decode checks  checks.json    sha256 71db5421ff190d052f8c004f10bbd0d618119546ea1711a1c0601e0ce2ad529a
//     second decode  decoded.tsv    sha256 19ebeefd5834a960ab8db6cd53549798c2c88ee4176fd7261e8f3cc991337d8e
// The second decode was made independently of the first and carries the same
// 44 codes; kCapture below is every row of the first, in order.
//
// The script (b counts bars from its first pulse bar; the source is na before
// bar 0 and from bar 46):
//     block = b / 40, ph = b % 40
//     a = 0.1 * (1 + block % 9)
//     v = 0.3 * (1 + (block / 9) % 7)
//     c = (block / 63) % 2 == 0 ? 0 : 0.7 * (1 + (block / 126) % 5)
//     x = ph == 0 ? a : ph == 1 ? v : ph == 2 ? c : 0
//     s = ta.sma(x, 3)
//     e = ((x + x[1]) + x[2]) / 3
//     code = ph >= 5 ? 10000000 + round(s * 1e18)
//                    : 50000000 + round((s - e) * 1e17)
// so x is 0.1, 0.3, 0, zeros to bar 39, then 0.2, 0.3, 0, zeros to bar 45,
// and each trade's entry comment is the code of the bar that placed it.
//
// What the rows pin:
//   * bar 3 (window 0.3, 0, 0) reads 50000000: the average is e itself,
//     0.3 / 3. Its two neighbouring doubles would read 49999999 and 50000001
//     (checked below), so the row holds the last bit of the average, and the
//     sum left when 0.1 leaves the window is exactly 0.3. The addition
//     0.1 + 0.3 rounds up by 2^-55. The textbook compensation (T - S) - y
//     reads 2^-54 there, because its first subtraction rounds when the
//     incoming 0.3 is larger than the sum 0.1; carried as 2^-54 it puts the
//     sum of bar 3 one unit in its last place below 0.3 and the row at
//     49999999;
//   * bars 5..39 and 45 read 10000000: an all-zero window is exactly 0 (the
//     smallest sum these sources can leave behind, 2^-56, would read
//     10000005);
//   * the other rows read 50000000, the script's own sum e.
//
// Controls. The 44 rows are the only TradingView observations in this file.
// The second part is a mathematical control, not a measurement: on a pulse
// p, q followed by zeros, with p below q in exponent and p far enough from the
// top of its binade for p plus the compensation to be a double, every step of
// the window sum is exact except the one rounding of p + q. math.sum must then
// read the correctly rounded exact sum of its window on every bar: fl(p + q)
// while both are inside, q once p has left, 0 once q has left. The reference
// adds the window in integers and rounds once, half to even; it performs no
// floating-point addition and does not use the engine. The pairs are the
// capture's 0.1 and 0.3 at three binades and three pairs of other mantissas
// one, three and four binades apart, each also negated, for window lengths 3
// and 5.

#include <pineforge/math.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define CHECK_TRUE(cond, tag) do {                                            \
    if (cond) { tests_passed++; }                                             \
    else { tests_failed++; std::printf("FAIL %s\n", (tag)); }                 \
} while (0)

// --- TradingView rows -------------------------------------------------------

struct Row {
    int bar;
    long long code;
};

const Row kCapture[] = {
    {2, 50000000},  {3, 50000000},  {4, 50000000},  {5, 10000000},  {6, 10000000},  {7, 10000000},
    {8, 10000000},  {9, 10000000},  {10, 10000000}, {11, 10000000}, {12, 10000000}, {13, 10000000},
    {14, 10000000}, {15, 10000000}, {16, 10000000}, {17, 10000000}, {18, 10000000}, {19, 10000000},
    {20, 10000000}, {21, 10000000}, {22, 10000000}, {23, 10000000}, {24, 10000000}, {25, 10000000},
    {26, 10000000}, {27, 10000000}, {28, 10000000}, {29, 10000000}, {30, 10000000}, {31, 10000000},
    {32, 10000000}, {33, 10000000}, {34, 10000000}, {35, 10000000}, {36, 10000000}, {37, 10000000},
    {38, 10000000}, {39, 10000000}, {40, 50000000}, {41, 50000000}, {42, 50000000}, {43, 50000000},
    {44, 50000000}, {45, 10000000},
};
constexpr int kRows = static_cast<int>(sizeof(kCapture) / sizeof(kCapture[0]));
constexpr int kFirstBar = 2;
constexpr int kLastBar = 45;

// x as the captured script computes it on bar b of its pulse window (b >= 0).
double pulse_source(int b) {
    const int block = b / 40;
    const int ph = b % 40;
    const double a = 0.1 * static_cast<double>(1 + block % 9);
    const double v = 0.3 * static_cast<double>(1 + (block / 9) % 7);
    const double c = (block / 63) % 2 == 0
        ? 0.0 : 0.7 * static_cast<double>(1 + (block / 126) % 5);
    return ph == 0 ? a : ph == 1 ? v : ph == 2 ? c : 0.0;
}

// The script's math.round on the values met here, none of which is a tie.
double pine_round(double value) {
    const double magnitude = std::floor(std::fabs(value) + 0.5);
    return value < 0 ? -magnitude : magnitude;
}

// The script's code for an average s on bar b, e being its own sum.
double pulse_code(int b, double s, double e) {
    return b % 40 >= 5 ? 10000000.0 + pine_round(s * 1e18)
                       : 50000000.0 + pine_round((s - e) * 1e17);
}

void test_captured_rows() {
    ta::SMA sma(3);
    // Before the first pulse bar the source is na and nothing enters.
    for (int bar = 0; bar < 3; ++bar) {
        CHECK_TRUE(is_na(sma.compute(na<double>())), "na before the first pulse bar");
    }
    double x[kLastBar + 1];
    double s[kLastBar + 1];
    for (int b = 0; b <= kLastBar; ++b) {
        x[b] = pulse_source(b);
        s[b] = sma.compute(x[b]);
    }
    CHECK_TRUE(is_na(s[0]) && is_na(s[1]), "bars 0 and 1 are na");

    bool in_order = kRows == kLastBar - kFirstBar + 1;
    int mismatches = 0;
    for (int row = 0; row < kRows; ++row) {
        const int b = kCapture[row].bar;
        if (b != kFirstBar + row) {
            in_order = false;
            continue;
        }
        const double e = ((x[b] + x[b - 1]) + x[b - 2]) / 3.0;
        const double code = pulse_code(b, s[b], e);
        if (code != static_cast<double>(kCapture[row].code)) {
            if (++mismatches <= 5) {
                std::printf("  bar %d: ta.sma = %.17g reads %.0f, TradingView %lld\n",
                            b, s[b], code, kCapture[row].code);
            }
        }
    }
    std::printf("  %d captured rows replayed, %d mismatches\n", kRows, mismatches);
    CHECK_TRUE(kRows == 44 && in_order, "the 44 rows of bars 2..45, in order");
    CHECK_TRUE(mismatches == 0, "ta.sma reproduces every captured row");
}

// What the rows resolve: arithmetic on the script's encoding only.
void test_rows_hold_the_last_bit() {
    const double e = ((0.0 + 0.0) + 0.3) / 3.0;
    CHECK_TRUE(pulse_code(3, e, e) == 50000000.0, "bar 3: the script's own sum reads 50000000");
    CHECK_TRUE(pulse_code(3, std::nextafter(e, 0.0), e) == 49999999.0,
               "bar 3: the double below it reads 49999999");
    CHECK_TRUE(pulse_code(3, std::nextafter(e, 1.0), e) == 50000001.0,
               "bar 3: the double above it reads 50000001");
    // Every source of the capture is a whole number of 2^-56, so a sum they
    // leave behind is 0 or at least 2^-56.
    CHECK_TRUE(pulse_code(5, std::ldexp(1.0, -56) / 3.0, 0.0) == 10000005.0,
               "bar 5: a sum of 2^-56 left in an all-zero window reads 10000005");
}

// --- Controls: mathematical, not TradingView observations -------------------

// units * 2^exponent as a double, rounded once, half to even, in integers.
double rounded(long long units, int exponent) {
    const bool negative = units < 0;
    unsigned long long magnitude = negative
        ? 0ULL - static_cast<unsigned long long>(units)
        : static_cast<unsigned long long>(units);
    int shift = 0;
    while ((magnitude >> shift) >= (1ULL << 53)) ++shift;
    if (shift > 0) {
        const unsigned long long kept = magnitude >> shift;
        const unsigned long long lost = magnitude & ((1ULL << shift) - 1ULL);
        const unsigned long long half = 1ULL << (shift - 1);
        const bool up = lost > half || (lost == half && (kept & 1ULL) != 0ULL);
        magnitude = kept + (up ? 1ULL : 0ULL);
    }
    const double value = std::ldexp(static_cast<double>(magnitude), exponent + shift);
    return negative ? -value : value;
}

// p = p_mantissa * 2^p_exponent enters first, q = q_mantissa * 2^q_exponent
// next; both mantissas have 53 bits and q is in a higher binade.
struct Pair {
    long long p_mantissa;
    int p_exponent;
    long long q_mantissa;
    int q_exponent;
};

const Pair kPairs[] = {
    // The capture's 0.1 and 0.3: forty binades down, in place, 37 binades up.
    {7205759403792794LL, -96, 5404319552844595LL, -94},
    {7205759403792794LL, -56, 5404319552844595LL, -54},
    {7205759403792794LL, -19, 5404319552844595LL, -17},
    // Other mantissas, one, three and four binades apart.
    {4503599627370497LL, -52, 6755399441055744LL, -51},
    {4503599627370499LL, -55, 9007199254740991LL, -52},
    {4503599627370501LL, -56, 5629499534213121LL, -52},
};
constexpr int kPairCount = static_cast<int>(sizeof(kPairs) / sizeof(kPairs[0]));
constexpr int kLongestWindow = 5;
constexpr int kMostBars = 2 * kPairCount * (kLongestWindow + 4);

double mantissa_times_power(long long mantissa, int exponent) {
    return std::ldexp(static_cast<double>(mantissa), exponent);
}

// The table meets the conditions under which the control is a theorem.
void test_control_pairs_meet_their_conditions() {
    CHECK_TRUE(mantissa_times_power(kPairs[1].p_mantissa, kPairs[1].p_exponent) == 0.1
                   && mantissa_times_power(kPairs[1].q_mantissa, kPairs[1].q_exponent) == 0.3,
               "control: the second pair is the capture's 0.1 and 0.3");
    const long long low = 1LL << 52;
    const long long high = 1LL << 53;
    for (const Pair& pair : kPairs) {
        const int gap = pair.q_exponent - pair.p_exponent;
        CHECK_TRUE(pair.p_mantissa >= low && pair.p_mantissa < high
                       && pair.q_mantissa >= low && pair.q_mantissa < high,
                   "control: both mantissas have 53 bits");
        CHECK_TRUE(gap >= 1 && gap <= 4, "control: q is one to four binades above p");
        // The rounding error of p + q is at most 2^gap units of p's last
        // place, so p plus the compensation is still a double.
        CHECK_TRUE(gap >= 1 && gap <= 4 && pair.p_mantissa + (1LL << gap) < high,
                   "control: p plus the compensation is a double");
    }
}

// One pulse per block of n + 4 bars: p, q, then zeros until both have left
// and two more all-zero windows have been read. The six pairs run in order,
// then again negated, through one math.sum, so every block also starts from
// what the block before it left.
void test_window_sum_is_the_rounded_exact_sum(int n) {
    const int block_bars = n + 4;
    const int bars = 2 * kPairCount * block_bars;
    double xs[kMostBars];
    int unit[kMostBars];
    for (int block = 0; block < 2 * kPairCount; ++block) {
        const Pair& pair = kPairs[block % kPairCount];
        const double sign = block < kPairCount ? 1.0 : -1.0;
        const double p = sign * mantissa_times_power(pair.p_mantissa, pair.p_exponent);
        const double q = sign * mantissa_times_power(pair.q_mantissa, pair.q_exponent);
        for (int ph = 0; ph < block_bars; ++ph) {
            const int bar = block * block_bars + ph;
            xs[bar] = ph == 0 ? p : ph == 1 ? q : 0.0;
            unit[bar] = pair.p_exponent;
        }
    }

    math::Sum sum(n);
    int checked = 0, mismatches = 0;
    bool warm_up_is_na = true;
    for (int bar = 0; bar < bars; ++bar) {
        const double got = sum.compute(xs[bar]);
        if (bar < n - 1) {
            warm_up_is_na = warm_up_is_na && is_na(got);
            continue;
        }
        // A block is longer than the window, so the window's non-zero sources
        // are all of this bar's block and its exact sum is a whole number of
        // that block's units.
        long long units = 0;
        for (int back = 0; back < n; ++back) {
            units += static_cast<long long>(std::ldexp(xs[bar - back], -unit[bar]));
        }
        const double want = rounded(units, unit[bar]);
        ++checked;
        if (got != want) {
            if (++mismatches <= 5) {
                std::printf("  length %d, bar %d: math.sum = %.17g, exact window sum rounded = %.17g\n",
                            n, bar, got, want);
            }
        }
    }
    std::printf("  control, length %d: %d window sums checked, %d mismatches\n",
                n, checked, mismatches);
    CHECK_TRUE(warm_up_is_na, "control: na until the window is full");
    CHECK_TRUE(checked == bars - (n - 1), "control: every full window is checked");
    CHECK_TRUE(mismatches == 0,
               n == 3 ? "control, length 3: math.sum is the rounded exact window sum"
                      : "control, length 5: math.sum is the rounded exact window sum");
}

}  // namespace

int main() {
    test_captured_rows();
    test_rows_hold_the_last_bit();
    test_control_pairs_meet_their_conditions();
    test_window_sum_is_the_rounded_exact_sum(3);
    test_window_sum_is_the_rounded_exact_sum(kLongestWindow);
    std::printf("test_ta_sma_exact_compensation: passed=%d failed=%d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
