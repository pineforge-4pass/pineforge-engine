// test_ta_sma_exact_compensation -- the window sum behind ta.sma and math.sum
// loses nothing: it is the exactly rounded sum of the sources in its window,
// whatever their order, signs and magnitudes, and a source that has left the
// window leaves nothing behind in it.
//
// TradingView rows: two captures of 2026-11-16 on OANDA:EURUSD 15 from
// 2025-04-01, the chart and start of the September pulse tapes.
//
// The pulse capture (the neighbouring SMA tests cite it as CAP-731-PULSE): 44
// closed trades and no open one, whose entry comments carry ta.sma(x, 3) of
// bars 2..45 of its pulse window.
//     script         strategy.pine  sha256 276834fe7b2c2f7c18888e7bf69469fe6aa8bcfe85a7961e0b2d419543b26443
//     trade list     tv_trades.csv  sha256 1e1a715be44e964f7db4475c6ce6074b98fcadc7db564737b63ad10662710a28
//     decoded rows   decoded.tsv    sha256 a149d349a4302b18eaef95be2ec7cf9d6327a1d3dd108a1fdde29f2f2a554cf5
//     decode checks  checks.json    sha256 71db5421ff190d052f8c004f10bbd0d618119546ea1711a1c0601e0ce2ad529a
//     second decode  decoded.tsv    sha256 19ebeefd5834a960ab8db6cd53549798c2c88ee4176fd7261e8f3cc991337d8e
// The second decode was made independently of the first and carries the same
// 44 codes; kCapture below is every row of the first, in order.
//
// Its script (b counts bars from its first pulse bar; the source is na before
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
// What its rows pin:
//   * bar 3 (window 0.3, 0, 0) reads 50000000: the average is e itself,
//     0.3 / 3. Its two neighbouring doubles would read 49999999 and 50000001
//     (checked below), so the row holds the last bit of the average, and the
//     sum left when 0.1 leaves the window is exactly 0.3;
//   * bars 5..39 and 45 read 10000000: an all-zero window is exactly 0 (the
//     smallest sum these sources can leave behind, 2^-56, would read
//     10000005);
//   * the other rows read 50000000, the script's own sum e.
//
// The fold capture (CAP-731-FOLD): four closed trades and no open one, on
// eight trade-list rows, whose comments carry three probes of bars 2..5.
//     script      strategy.pine        sha256 776a0add5887f44e5e3e5e578b8b4a97b7763c6d4a80483f8fc3cb6c03c1640b
//     trade list  tv_trades.csv        sha256 88f05d8bd9981dad5796edbc4a4517c0445541ddc0d5d6e66db7381dffabd638
//     report      decoded-report.json  sha256 3b137ee521c3fc0e21a1d07043d0eb0a5242ff742f0818546181d42c0330de6c
// Its script (b counts bars from its first source bar; both sources are na
// before bar 0 and from bar 6):
//     x        = b == 0 ? 0.5 : b == 1 ? 0.3 : 0
//     controlX = b == 0 ? 0.1 : b == 1 ? 0.3 : 0
//     s = ta.sma(x, 3), m = math.sum(x, 3) / 3, c = ta.sma(controlX, 3)
//     e = ((x + x[1]) + x[2]) / 3, controlE the same on controlX
//     code = b >= 5 ? 10000000 + round(value * 1e18)
//                   : 50000000 + round((value - reference) * 1e17)
// with reference e for s and m and controlE for c. The fold regression pins the comment
// of each trade, which the trade list carries on the trade's entry row and
// again on its exit row; every probe of every row has na=0 and u=0. Bars 0
// and 1 are not exported, so nothing is read about them here.
//
// What its rows pin:
//   * bars 3 and 4 read 50000000 and bar 5 reads 10000000, for ta.sma and for
//     math.sum / 3 alike: the window (0.3, 0, 0) sums to 0.3 and an all-zero
//     window to exactly 0. 0.5 + 0.3 rounds up by 2^-54. A running sum that
//     carries that as a compensation can hand it neither to the zero of bar 2
//     (0.8 - 2^-54 rounds back to 0.8) nor to the departing 0.5 (0.5 + 2^-54
//     rounds to 0.5), so it reads 0.3 + 2^-54 on bar 3 and 2^-54 from bar 4
//     on, which the script would encode as 50000003, 50000002 and 10000019
//     (checked below on the encoding alone);
//   * bar 2 reads 50000000: 0.5 + 0.3 rounded once, as the script's own sum;
//   * the control probe repeats bars 2..5 of the pulse capture with the same
//     codes.
//
// Controls. The rows above are the only TradingView observations in this
// file. The rest is mathematics, not measurement, and none of its expected
// values comes from the engine:
//   * pulse pairs: a pulse p, q followed by zeros, q in a higher binade than
//     p. The window holds p and q, then q, then nothing, so math.sum must read
//     fl(p + q), q and 0. The reference adds the window in integers and
//     rounds once, half to even; it performs no floating-point addition. The
//     pairs are the pulse capture's 0.1 and 0.3 at three binades and three
//     pairs of other mantissas one, three and four binades apart, each also
//     negated, for window lengths 3 and 5;
//   * any window: a fixed series of 53-bit sources over five binades, of both
//     signs and with zeros, beginning with the fold capture's 0.5, 0.3 and
//     zeros, for window lengths 1, 2, 3, 5 and 20, as given and negated.
//     Every source is a whole number of 2^-54, so the same integer reference
//     applies to every window;
//   * rewound bars: that series again, each bar first given a provisional
//     source of another magnitude, or an infinity, and then recomputed,
//     withdrawn (na) and recomputed, or restored from a copy taken before it;
//   * the rounded sum is what ta.sma divides: three sources whose exact sum
//     is 3 * 2^53 + 3 average to 2^53 + 2, where the rounded exact mean and a
//     plain double sum both read 2^53;
//   * extreme windows, each derived by hand: cancellation across 600 binades,
//     ties and the smallest subnormal as a tie-breaker, overflow and the
//     return from it, subnormal sums, signed zeros, and non-finite sources
//     entering, leaving and being rewound.

#include <pineforge/math.hpp>
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/window_sum.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <limits>

using namespace pineforge;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define CHECK_TRUE(cond, tag) do {                                            \
    if (cond) { tests_passed++; }                                             \
    else { tests_failed++; std::printf("FAIL %s\n", (tag)); }                 \
} while (0)

// --- TradingView rows: the pulse capture ------------------------------------

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

// The scripts' math.round on the values met here, none of which is a tie.
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

// What the fold rows resolve: arithmetic on the script's encoding only. The
// values are those a running sum reads when it keeps the 2^-54 by which
// 0.5 + 0.3 rounds up (the header derives them).
void test_fold_rows_exclude_a_carried_remainder() {
    const double remainder = std::ldexp(1.0, -54);
    const double e = ((0.0 + 0.0) + 0.3) / 3.0;
    CHECK_TRUE(pulse_code(3, e, e) == 50000000.0, "fold bar 3: 0.3 / 3 reads 50000000");
    CHECK_TRUE(pulse_code(3, (0.3 + remainder) / 3.0, e) == 50000003.0,
               "fold bar 3: a sum of 0.3 + 2^-54 reads 50000003");
    CHECK_TRUE(pulse_code(4, remainder / 3.0, 0.0) == 50000002.0,
               "fold bar 4: a sum of 2^-54 left in an all-zero window reads 50000002");
    CHECK_TRUE(pulse_code(5, remainder / 3.0, 0.0) == 10000019.0,
               "fold bar 5: a sum of 2^-54 left in an all-zero window reads 10000019");
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

// The table is what the header says it is, and its sums fit the reference.
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
        // place, so p plus that error is still a double: a running sum that
        // carried the error exactly would also be right on these pairs.
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

// --- Control: any window ----------------------------------------------------

// The series of the two controls below, as whole numbers of 2^-54: the fold
// capture's 0.5 and 0.3 and four zeros, then 300 sources of 53 significant
// bits spread over five binades, of either sign, one in eight of them zero.
// A window of at most 20 of them sums to less than 2^62 units.
constexpr int kSeriesUnit = -54;
constexpr int kSeriesBars = 306;
constexpr int kSeriesLongestWindow = 20;

// One step of a fixed 64-bit linear congruential sequence (Knuth's MMIX
// constants): the series is the same on every run and every platform.
unsigned long long next_word(unsigned long long& state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
}

void series_units(long long* units) {
    units[0] = 1LL << 53;             // 0.5
    units[1] = 5404319552844595LL;    // 0.3
    for (int bar = 2; bar < 6; ++bar) units[bar] = 0;
    unsigned long long state = 731ULL;
    for (int bar = 6; bar < kSeriesBars; ++bar) {
        const unsigned long long first = next_word(state);
        const unsigned long long second = next_word(state);
        const long long mantissa = static_cast<long long>((first >> 11) | (1ULL << 52));
        const int binade = static_cast<int>((second >> 60) % 5ULL);
        const bool negative = ((second >> 59) & 1ULL) != 0ULL;
        const bool zero = ((second >> 50) % 8ULL) == 0ULL;
        const long long magnitude = mantissa * (1LL << binade);
        units[bar] = zero ? 0 : negative ? -magnitude : magnitude;
    }
}

double series_source(long long units) {
    return std::ldexp(static_cast<double>(units), kSeriesUnit);
}

// The exact sum of the n sources ending at `bar`, rounded once.
double series_window(const long long* units, int bar, int n) {
    long long total = 0;
    for (int back = 0; back < n; ++back) total += units[bar - back];
    return rounded(total, kSeriesUnit);
}

void test_series_starts_with_the_fold_capture() {
    long long units[kSeriesBars];
    series_units(units);
    CHECK_TRUE(series_source(units[0]) == 0.5 && series_source(units[1]) == 0.3
                   && series_source(units[2]) == 0.0 && series_source(units[5]) == 0.0,
               "control: the series begins with the fold capture's 0.5, 0.3 and zeros");
    bool fits = true;
    int zeros = 0, negatives = 0;
    for (int bar = 0; bar < kSeriesBars; ++bar) {
        const long long magnitude = units[bar] < 0 ? -units[bar] : units[bar];
        fits = fits && magnitude < (1LL << 57);
        zeros += units[bar] == 0;
        negatives += units[bar] < 0;
    }
    CHECK_TRUE(fits, "control: every source is below 2^57 units, so 20 of them fit 64 bits");
    CHECK_TRUE(zeros > 10 && negatives > 50 && negatives < kSeriesBars - 50,
               "control: the series holds zeros and both signs");
}

// math.sum on every full window of the series, as given and negated, and
// ta.sma as that sum divided by the length.
void test_any_window_sum_is_exactly_rounded(int n) {
    long long units[kSeriesBars];
    series_units(units);
    int checked = 0, sum_mismatches = 0, sma_mismatches = 0;
    bool warm_up_is_na = true;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            for (int bar = 0; bar < kSeriesBars; ++bar) units[bar] = -units[bar];
        }
        math::Sum sum(n);
        ta::SMA sma(n);
        for (int bar = 0; bar < kSeriesBars; ++bar) {
            const double x = series_source(units[bar]);
            const double got = sum.compute(x);
            const double average = sma.compute(x);
            if (bar < n - 1) {
                warm_up_is_na = warm_up_is_na && is_na(got) && is_na(average);
                continue;
            }
            const double want = series_window(units, bar, n);
            ++checked;
            if (got != want) {
                if (++sum_mismatches <= 5) {
                    std::printf("  length %d, %s series, bar %d: math.sum = %.17g,"
                                " exact window sum rounded = %.17g\n",
                                n, pass == 0 ? "given" : "negated", bar, got, want);
                }
            }
            if (average != want / n) ++sma_mismatches;
        }
    }
    std::printf("  any window, length %d: %d window sums checked, %d math.sum and"
                " %d ta.sma mismatches\n", n, checked, sum_mismatches, sma_mismatches);
    CHECK_TRUE(n >= 1 && n <= kSeriesLongestWindow, "any window: the length fits the reference");
    CHECK_TRUE(warm_up_is_na, "any window: na until the window is full");
    CHECK_TRUE(checked == 2 * (kSeriesBars - (n - 1)), "any window: every full window is checked");
    CHECK_TRUE(sum_mismatches == 0, "any window: math.sum is the rounded exact window sum");
    CHECK_TRUE(sma_mismatches == 0, "any window: ta.sma is that sum divided by the length");
}

// --- Control: rewound bars --------------------------------------------------

// Every bar is first given a provisional source that is not in the series,
// then its own. For ta.sma the bar is recomputed. For math.sum two bars in
// three are withdrawn (an na recompute, which must read the window of the bar
// before) and recomputed, and the third is restored from a copy taken before
// the bar, as a fill replay restores a checkpoint. Whatever the provisional
// source was, the bar must read the exact sum of its window.
void test_rewound_bars_leave_nothing(int n) {
    long long units[kSeriesBars];
    series_units(units);
    const double infinity = std::numeric_limits<double>::infinity();
    const double provisional[] = {
        std::ldexp(1.5, 900), -std::ldexp(1.25, -900), infinity, -infinity,
        std::numeric_limits<double>::max(), -std::numeric_limits<double>::denorm_min(),
    };
    math::Sum sum(n);
    ta::SMA sma(n);
    int checked = 0, mismatches = 0;
    for (int bar = 0; bar < kSeriesBars; ++bar) {
        const double x = series_source(units[bar]);
        const double other = provisional[bar % 6];
        const bool full = bar >= n - 1;
        const double want = full ? series_window(units, bar, n) : na<double>();

        sma.compute(other);
        const double average = sma.recompute(x);

        bool withdrawn_ok = true;
        double got = na<double>();
        if (bar % 3 == 0) {
            const math::Sum before = sum;
            sum.compute(other);
            sum = before;
            got = sum.compute(x);
        } else {
            sum.compute(other);
            const double withdrawn = sum.recompute(na<double>());
            withdrawn_ok = bar >= n ? withdrawn == series_window(units, bar - 1, n)
                                    : is_na(withdrawn);
            got = sum.recompute(x);
        }

        ++checked;
        const bool ok = full ? (got == want && average == want / n)
                             : (is_na(got) && is_na(average));
        if (!ok || !withdrawn_ok) {
            if (++mismatches <= 5) {
                std::printf("  length %d, bar %d: math.sum = %.17g, ta.sma = %.17g,"
                            " exact window sum rounded = %.17g%s\n", n, bar, got, average, want,
                            withdrawn_ok ? "" : " (the withdrawn bar read another window)");
            }
        }
    }
    std::printf("  rewound bars, length %d: %d bars checked, %d mismatches\n",
                n, checked, mismatches);
    CHECK_TRUE(checked == kSeriesBars, "rewound bars: every bar is checked");
    CHECK_TRUE(mismatches == 0, "rewound bars: a replaced, withdrawn or restored bar leaves nothing");
}

// --- Control: the rounded sum is what ta.sma divides ------------------------

// 2^53 + 2, 2^53 - 1, 2^53 + 2 sum exactly to 3 * 2^53 + 3. Doubles are 4
// apart there, so the sum rounds to 3 * 2^53 + 4, and a third of that is
// 2^53 + 4/3, which rounds to 2^53 + 2. The exact mean is 2^53 + 1, halfway
// between 2^53 and 2^53 + 2, and rounds to the even 2^53. A plain double sum
// loses the odd unit of (2^53 + 2) + (2^53 - 1), meets a tie at 3 * 2^53 + 2
// and reads 3 * 2^53, whose third is 2^53 as well.
void test_rounded_sum_is_what_is_divided() {
    const double two53 = std::ldexp(1.0, 53);
    const double outer = two53 + 2.0;
    const double middle = two53 - 1.0;
    const long long total = 2 * ((1LL << 53) + 2) + ((1LL << 53) - 1);
    CHECK_TRUE(total == 3 * ((1LL << 53) + 1), "division: the exact mean is 2^53 + 1");
    CHECK_TRUE(rounded(total, 0) == 3.0 * two53 + 4.0, "division: the exact sum rounds to 3 * 2^53 + 4");
    CHECK_TRUE(rounded(total, 0) / 3.0 == two53 + 2.0, "division: a third of it is 2^53 + 2");
    CHECK_TRUE(((outer + middle) + outer) / 3.0 == two53, "division: a plain double sum reads 2^53");

    ta::SMA sma(3);
    sma.compute(outer);
    sma.compute(middle);
    const double average = sma.compute(outer);
    if (average != two53 + 2.0) {
        std::printf("  division: ta.sma = %.17g, rounded exact sum / 3 = %.17g\n",
                    average, two53 + 2.0);
    }
    CHECK_TRUE(average == two53 + 2.0, "division: ta.sma divides the rounded exact sum");
}

// --- Control: extreme windows -----------------------------------------------

// Equal as doubles and in the sign of a zero; na equals na.
bool same_double(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return a == b && std::signbit(a) == std::signbit(b);
}

// A source pushed into the window and the sum the window must then read.
struct Step {
    double source;
    double sum;
};

void check_steps(int n, const Step* steps, int count, const char* tag) {
    KahanWindowSum window(n);
    int mismatches = 0;
    for (int step = 0; step < count; ++step) {
        const double got = window.push(steps[step].source);
        if (!same_double(got, steps[step].sum)) {
            ++mismatches;
            std::printf("  %s, step %d: sum = %.17g, exact window sum rounded = %.17g\n",
                        tag, step, got, steps[step].sum);
        }
    }
    CHECK_TRUE(mismatches == 0, tag);
}

template <std::size_t Count>
void check_steps(int n, const Step (&steps)[Count], const char* tag) {
    check_steps(n, steps, static_cast<int>(Count), tag);
}

void test_extreme_windows() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double most = std::numeric_limits<double>::max();
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double two53 = std::ldexp(1.0, 53);
    const double big = std::ldexp(1.0, 600);

    // 2^600 + 1 rounds to 2^600, yet the 1 is still there when -2^600 joins
    // it. Then the window is (1, -2^600, 0), which rounds to -2^600, then
    // (-2^600, 0, 0), then all zeros.
    const Step cancellation[] = {
        {big, big}, {1.0, big}, {-big, 1.0}, {0.0, -big}, {0.0, -big}, {0.0, 0.0},
    };
    check_steps(3, cancellation, "extreme: cancellation across 600 binades");

    // 2^53 + 1 is halfway between 2^53 and 2^53 + 2 and goes to the even
    // 2^53. The smallest subnormal above it tips the sum up, below it down.
    const Step above_a_tie[] = {{two53, two53}, {1.0, two53}, {tiny, two53 + 2.0}};
    check_steps(3, above_a_tie, "extreme: the smallest subnormal lifts a tie");
    const Step below_a_tie[] = {{two53, two53}, {1.0, two53}, {-tiny, two53}};
    check_steps(3, below_a_tie, "extreme: the smallest subnormal lowers a tie");
    // 2^53 + 3 is halfway between 2^53 + 2 and the even 2^53 + 4.
    const Step tie_to_even_upward[] = {{two53 + 2.0, two53 + 2.0}, {1.0, two53 + 4.0}};
    check_steps(2, tie_to_even_upward, "extreme: a tie goes to the even side, also upward");

    // Twice the largest double overflows; with its negation in the window the
    // exact sum is the largest double again, and then zero.
    const Step overflow[] = {{most, most}, {most, infinity}, {-most, most}, {0.0, 0.0}};
    check_steps(3, overflow, "extreme: overflow, and the finite sum after it");
    // The largest double plus half its last place (2^970) is halfway to 2^1024
    // and overflows, as an IEEE addition does; plus 2^969 it stays.
    const Step half_way_up[] = {{most, most}, {std::ldexp(1.0, 970), infinity}};
    check_steps(2, half_way_up, "extreme: halfway past the largest double overflows");
    const Step under_half_way[] = {{most, most}, {std::ldexp(1.0, 969), most}};
    check_steps(2, under_half_way, "extreme: under halfway past the largest double stays");

    // Subnormal sums are exact: one, two and three units of 2^-1074, then the
    // windows (1, 1, -1), (1, -1, -1) and (-1, -1, -1) of that unit.
    const Step subnormal[] = {
        {tiny, tiny}, {tiny, 2.0 * tiny}, {tiny, 3.0 * tiny},
        {-tiny, tiny}, {-tiny, -tiny}, {-tiny, -3.0 * tiny},
    };
    check_steps(3, subnormal, "extreme: subnormal sums are exact");

    // An exact zero is +0: a window of negative zeros, and a source with its
    // negation.
    const Step zeros[] = {{-0.0, 0.0}, {-0.0, 0.0}, {-0.0, 0.0}, {0.1, 0.1}, {-0.1, 0.0}};
    check_steps(3, zeros, "extreme: an exact zero is +0");

    // Windows of three over 1, +inf, 2, 3, 4, -inf, +inf, 5, 6, 7: an infinity
    // decides the sum while it is in the window, both signs together are NaN,
    // and the finite sum is exact again once they have left.
    const Step non_finite[] = {
        {1.0, 1.0}, {infinity, infinity}, {2.0, infinity}, {3.0, infinity}, {4.0, 9.0},
        {-infinity, -infinity}, {infinity, na<double>()}, {5.0, na<double>()},
        {6.0, infinity}, {7.0, 18.0},
    };
    check_steps(3, non_finite, "extreme: infinities enter and leave");
    // The indicators never push a NaN (they hold their value on na input); one
    // pushed directly is carried like an infinity and leaves like one.
    const Step nan_source[] = {
        {1.0, 1.0}, {na<double>(), na<double>()}, {2.0, na<double>()}, {3.0, 5.0},
    };
    check_steps(2, nan_source, "extreme: a NaN source leaves with its bar");
}

// Rewinding a bar gives an evicted infinity back to the window, and takes a
// provisional one out of it.
void test_non_finite_sources_rewind() {
    const double infinity = std::numeric_limits<double>::infinity();
    KahanWindowSum window(2);
    window.push(1.0);
    window.push(2.0);
    bool ok = same_double(window.push(infinity), infinity);     // (2, +inf)
    window.unpush();                                            // (1, 2)
    ok = ok && same_double(window.sum(), 3.0);
    ok = ok && same_double(window.push(4.0), 6.0);              // (2, 4)
    ok = ok && same_double(window.repush(-infinity), -infinity);  // (2, -inf)
    ok = ok && same_double(window.repush(8.0), 10.0);           // (2, 8)
    CHECK_TRUE(ok, "rewind: a provisional infinity leaves no trace");

    ok = same_double(window.push(infinity), infinity);          // (8, +inf)
    ok = ok && same_double(window.push(16.0), infinity);        // (+inf, 16)
    ok = ok && same_double(window.push(32.0), 48.0);            // (16, 32)
    window.unpush();                                            // (+inf, 16)
    ok = ok && same_double(window.sum(), infinity);
    ok = ok && same_double(window.push(64.0), 80.0);            // (16, 64)
    CHECK_TRUE(ok, "rewind: an evicted infinity returns to the window");
}

}  // namespace

int main() {
    test_captured_rows();
    test_rows_hold_the_last_bit();
    test_fold_rows_exclude_a_carried_remainder();
    test_control_pairs_meet_their_conditions();
    test_window_sum_is_the_rounded_exact_sum(3);
    test_window_sum_is_the_rounded_exact_sum(kLongestWindow);
    test_series_starts_with_the_fold_capture();
    for (int n : {1, 2, 3, 5, kSeriesLongestWindow}) test_any_window_sum_is_exactly_rounded(n);
    for (int n : {1, 3, kSeriesLongestWindow}) test_rewound_bars_leave_nothing(n);
    test_rounded_sum_is_what_is_divided();
    test_extreme_windows();
    test_non_finite_sources_rewind();
    std::printf("test_ta_sma_exact_compensation: passed=%d failed=%d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
