#include <pineforge/source/pine_ta_series_osc.hpp>
#include <pineforge/source/pine_ta_cci.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/detail/ta_lookback.hpp>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/ta.hpp>
#include <pineforge/ta_compare_band.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// The series-length classes of ta.stoch, ta.cci, ta.mfi, ta.mom, ta.roc,
// ta.cmo, ta.wpr, ta.cog, ta.rising and ta.falling
// (<pineforge/source/pine_ta_series_osc.hpp>). Every tape below is invented.
//
// Target 1 (constant_length): a per-call length that never changes gives the
// fixed-length class's value on every bar, bit for bit, na bars included.
// For ta.mfi and ta.cci the fixed-length class is the Pine call's own type,
// PineMfi and PineCci, and the tapes hold the case each of the two exists
// for: ta.mfi's first value, on the call that completes the first `length`
// calls, and ta.cci's 0 on a window whose mean deviation is zero only up to
// rounding. pine_type_rows counts those rows, so a tape without them is not
// a pass.
// Target 2 (changing_length): a length that changes from bar to bar gives the
// function recomputed directly over the last `length` bars of the tape. For
// ta.stoch and ta.wpr on highs and lows that hold an na the window extremes
// are written by hand (changing_length_na_extremes).
// The remaining functions cover what the two targets rest on: the length
// check, recompute(), a lookback cap and a checkpoint copy.

#if !defined(PINEFORGE_HAS_PINE_SERIES_STOCH) || !defined(PINEFORGE_HAS_PINE_SERIES_CCI) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_MFI) || !defined(PINEFORGE_HAS_PINE_SERIES_MOM) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_ROC) || !defined(PINEFORGE_HAS_PINE_SERIES_CMO) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_WPR) || !defined(PINEFORGE_HAS_PINE_SERIES_COG) || \
    !defined(PINEFORGE_HAS_PINE_SERIES_RISING) || !defined(PINEFORGE_HAS_PINE_SERIES_FALLING)
#error "pine_ta_series_osc.hpp must define one feature macro per series class"
#endif

namespace {

namespace ta = pineforge::ta;
using pineforge::na;

const double NA = na<double>();

struct Tapes {
    std::vector<double> source;   // also the close of ta.wpr
    std::vector<double> high;
    std::vector<double> low;
    std::vector<double> volume;
    std::size_t bars() const { return source.size(); }
};

// 24 invented bars. The source has two na bars, a repeated value (bars 7-8
// and 18-19), a rising run (bars 10-13), a falling run (bars 13-17) and a
// zero (bar 20). The high has an na at bar 6 and the low at bar 8; bar 19 has
// no range. The volume has an na at bar 15.
Tapes na_tape() {
    Tapes t;
    t.source = {20.5, 21.25, 21.0, 22.75, 23.5, NA,   22.0,  24.25, 24.25, 23.0, 25.5,  26.0,
                26.75, 27.5, 26.5, 25.75, 25.0, 24.5, 27.0,  27.0,  0.0,   28.25, NA,   29.5};
    t.high = {21.0, 21.75, 21.5, 23.0, 24.0, 23.75, NA,   24.5, 24.5,  23.75, 26.0, 26.5,
              27.0, 28.0,  27.0, 26.0, 25.5, 25.0,  27.5, 27.0, 27.25, 28.5,  29.0, 30.0};
    t.low = {20.0, 20.75, 20.5, 22.0, 23.0, 22.5, 21.5, 23.5, NA,    22.5, 25.0, 25.5,
             26.0, 27.0,  26.0, 25.0, 24.5, 24.0, 26.5, 27.0, 26.25, 27.5, 28.0, 29.0};
    t.volume = {1200, 950,  1100, 1500, 1300, 800,  1000, 1700, 900, 1250, 2100, 1600,
                1400, 1900, 1150, NA,   1350, 1000, 2200, 700,  650, 1800, 1050, 2000};
    return t;
}

// The same bars with a value in place of the high's and the low's na.
Tapes clean_extremes_tape() {
    Tapes t = na_tape();
    t.high[6] = 23.0;
    t.low[8] = 23.25;
    return t;
}

// 12 invented bars whose only na bars come first.
Tapes leading_na_tape() {
    Tapes t;
    t.source = {NA, NA, 20.5, 21.25, 21.0, 22.75, 0.0, 23.5, 22.0, 24.25, 24.25, 23.0};
    t.high.assign(t.source.size(), 30.0);
    t.low.assign(t.source.size(), 10.0);
    t.volume.assign(t.source.size(), 1000.0);
    return t;
}

// 64 generated bars (quarter steps, so every value is exact) with na bars in
// each input on a different cadence.
Tapes generated_tape() {
    Tapes t;
    for (std::size_t i = 0; i < 64; ++i) {
        const double base = 40.0 + static_cast<double>((i * 37) % 23) * 0.25 -
                            static_cast<double>((i * 11) % 7) * 0.5;
        t.source.push_back(i % 13 == 6 ? NA : base);
        t.high.push_back(i % 17 == 9 ? NA : base + 0.75);
        t.low.push_back(i % 19 == 11 ? NA : base - 0.5);
        t.volume.push_back(i % 23 == 14 ? NA : 500.0 + static_cast<double>((i * 53) % 31) * 10.0);
    }
    return t;
}

// 12 invented bars for ta.mfi's first value. The first bar has a volume of 0,
// so the first call's flow is 0 on both sides, and bar 2, which rises, has no
// volume. The first value is therefore 100 for the lengths 1 and 2 (a lower
// sum of 0) and na for every longer length (an upper sum that holds the
// missing volume, beside a lower sum that is 0 for length 3).
Tapes zero_first_flow_tape() {
    Tapes t;
    t.source = {30.0, 30.5, 31.25, 30.75, 30.75, 32.0, 31.0, 33.25, 32.5, 34.0, 33.0, 35.5};
    t.volume = {0.0, 400.0, NA, 250.0, 300.0, 350.0, 150.0, 500.0, 450.0, 200.0, 600.0, 100.0};
    t.high.assign(t.source.size(), 40.0);
    t.low.assign(t.source.size(), 20.0);
    return t;
}

// 24 invented bars for ta.cci on a window that is flat up to rounding: 0.1 on
// most bars, a 0.4 at bar 8, an na at bar 12 and 0.7 on bars 16 to 19. Three
// values of 0.1 sum to 0.30000000000000004, so their mean is one step above
// 0.1 and their mean deviation is that step, not 0. Other windows of equal
// values round to an exact 0 or leave such a step; every row is compared with
// PineCci either way.
Tapes flat_window_tape() {
    Tapes t;
    t.source = {0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.4, 0.1, 0.1, 0.1,
                NA,  0.1, 0.1, 0.1, 0.7, 0.7, 0.7, 0.7, 0.1, 0.1, 0.1, 0.1};
    t.high.assign(t.source.size(), 1.0);
    t.low.assign(t.source.size(), 0.0);
    t.volume.assign(t.source.size(), 1000.0);
    return t;
}

// One length per bar of the 24-bar tapes: it grows, shrinks and repeats, is
// longer than the history at the start (bar 0, and bar 3 for the functions
// that read `length` bars back) and carries two fractions, which truncate
// toward zero.
const std::vector<double> kChangingLengths = {3, 1, 2, 4, 2.9, 5, 3, 1, 4, 2,    6, 3,
                                              5, 2, 4, 1, 3,   7, 2, 5, 3, 4.75, 2, 6};

int g_failures = 0;
std::size_t g_rows = 0;

bool same_bits(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

void check(const char* what, const char* target, std::size_t bar, double length, double actual,
           double expected) {
    ++g_rows;
    if (same_bits(actual, expected)) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s %s bar=%zu length=%g actual=%a expected=%a\n", what, target,
                 bar, length, actual, expected);
}

// --- the functions recomputed directly over the last `length` bars ---

double direct_mom(const std::vector<double>& s, std::size_t i, std::size_t n) {
    if (i < n) return NA;
    if (std::isnan(s[i]) || std::isnan(s[i - n])) return NA;
    return s[i] - s[i - n];
}

double direct_roc(const std::vector<double>& s, std::size_t i, std::size_t n) {
    if (i < n) return NA;
    if (std::isnan(s[i]) || std::isnan(s[i - n]) || s[i - n] == 0.0) return NA;
    return (s[i] - s[i - n]) / s[i - n] * 100.0;
}

double direct_cog(const std::vector<double>& s, std::size_t i, std::size_t n) {
    if (i + 1 < n) return NA;
    double num = 0;
    double den = 0;
    for (std::size_t k = 0; k < n; ++k) num += s[i - k] * static_cast<double>(k + 1);
    for (std::size_t j = i + 1 - n; j <= i; ++j) den += s[j];
    if (den == 0) return NA;
    return -num / den;
}

double direct_cmo(const std::vector<double>& s, std::size_t i, std::size_t n) {
    if (i < n) return NA;
    double up_sum = 0;
    double down_sum = 0;
    for (std::size_t j = i + 1 - n; j <= i; ++j) {
        double up = 0;
        double down = 0;
        if (!std::isnan(s[j - 1])) {
            const double diff = s[j] - s[j - 1];
            if (diff > 0) up = diff;
            else down = -diff;
        }
        up_sum += up;
        down_sum += down;
    }
    const double denom = up_sum + down_sum;
    if (denom == 0) return NA;
    return 100.0 * (up_sum - down_sum) / denom;
}

double direct_mfi(const std::vector<double>& s, const std::vector<double>& v, std::size_t i,
                  std::size_t n) {
    if (i + 1 < n) return NA;
    if (i + 1 == n) {
        // The first value: bar 0 has no bar before it and counts on both
        // sides, bars 1 .. i count by direction, and an na sum is na.
        double upper = s[0] * v[0];
        double lower = upper;
        for (std::size_t j = 1; j <= i; ++j) {
            const double flow = s[j] * v[j];
            if (pineforge::float_band_gt(s[j], s[j - 1])) upper += flow;
            else if (pineforge::float_band_lt(s[j], s[j - 1])) lower += flow;
        }
        if (std::isnan(upper) || std::isnan(lower)) return NA;
        if (lower == 0) return 100.0;
        return 100.0 - 100.0 / (1.0 + upper / lower);
    }
    double pos_sum = 0;
    double neg_sum = 0;
    for (std::size_t j = i + 1 - n; j <= i; ++j) {
        double pos = 0;
        double neg = 0;
        if (!std::isnan(s[j - 1])) {
            const double flow = s[j] * v[j];
            if (pineforge::float_band_gt(s[j], s[j - 1])) pos = flow;
            else if (pineforge::float_band_lt(s[j], s[j - 1])) neg = flow;
        }
        pos_sum += pos;
        neg_sum += neg;
    }
    if (neg_sum == 0) return 100.0;
    return 100.0 - 100.0 / (1.0 + pos_sum / neg_sum);
}

// The last `count` non-na values up to bar i, oldest first: empty when bar i
// is na or fewer than `count` exist.
std::vector<double> present(const std::vector<double>& s, std::size_t i, std::size_t count) {
    std::vector<double> out;
    if (std::isnan(s[i])) return out;
    for (std::size_t j = i + 1; j > 0 && out.size() < count; --j) {
        if (!std::isnan(s[j - 1])) out.push_back(s[j - 1]);
    }
    if (out.size() < count) {
        out.clear();
        return out;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

double direct_cci(const std::vector<double>& s, std::size_t i, std::size_t n) {
    const std::vector<double> window = present(s, i, n);
    if (window.empty()) return NA;
    double sum = 0.0;
    for (double value : window) sum += value;
    const double mean = sum / static_cast<double>(n);
    double md_sum = 0.0;
    for (double value : window) md_sum += std::abs(value - mean);
    const double mean_dev = md_sum / static_cast<double>(n);
    // Zero up to rounding: at most 1e-10 from zero, the comparison band.
    if (pineforge::float_band_eq(mean_dev, 0.0)) return 0.0;
    return (s[i] - mean) / (0.015 * mean_dev);
}

double direct_rising(const std::vector<double>& s, std::size_t i, std::size_t n) {
    const std::vector<double> window = present(s, i, n + 1);
    if (window.empty()) return 0.0;
    for (std::size_t k = 1; k < window.size(); ++k) {
        if (window[k] <= window[k - 1]) return 0.0;
    }
    return 1.0;
}

double direct_falling(const std::vector<double>& s, std::size_t i, std::size_t n) {
    const std::vector<double> window = present(s, i, n + 1);
    if (window.empty()) return 0.0;
    for (std::size_t k = 1; k < window.size(); ++k) {
        if (window[k] >= window[k - 1]) return 0.0;
    }
    return 1.0;
}

// The highest high and the lowest low of the last `n` bars. The highs and
// lows these two references read hold no na (clean_extremes_tape).
bool direct_extremes(const Tapes& t, std::size_t i, std::size_t n, double& hi, double& lo) {
    if (i + 1 < n) return false;
    hi = t.high[i + 1 - n];
    lo = t.low[i + 1 - n];
    for (std::size_t j = i + 2 - n; j <= i; ++j) {
        hi = std::max(hi, t.high[j]);
        lo = std::min(lo, t.low[j]);
    }
    return true;
}

double direct_stoch(const Tapes& t, std::size_t i, std::size_t n) {
    double hi = 0;
    double lo = 0;
    if (!direct_extremes(t, i, n, hi, lo) || std::isnan(t.source[i])) return NA;
    const double range = hi - lo;
    if (range == 0.0) return NA;
    return 100.0 * (t.source[i] - lo) / range;
}

double direct_wpr(const Tapes& t, std::size_t i, std::size_t n) {
    double hh = 0;
    double ll = 0;
    if (!direct_extremes(t, i, n, hh, ll) || hh == ll) return NA;
    return (hh - t.source[i]) / (hh - ll) * -100.0;
}

// --- the window extremes on highs and lows that hold an na, written by hand ---

// The highest high and the lowest low ta.stoch and ta.wpr read on each bar of
// na_tape() with kChangingLengths (n is the bar's length, fractions
// truncated). No class computed these numbers: each was worked out on paper
// from the rule documented at SeriesWindowExtreme (pine_ta_length.hpp). The
// extremum is kept as a cached {value, bar}:
//
//   * a call answers na while fewer than n bars exist, and on an na source,
//     which also drops the cache;
//   * a call whose length is longer than the call before first reads the
//     bars the longer window adds, oldest first, into the cache: a strictly
//     better value takes it, an na drops it, and the next value read starts
//     it again;
//   * then the source: without a cache the call answers its own source and
//     keeps nothing; a strictly better source takes the cache; a cache n or
//     more bars old is replaced by a read of the whole window, oldest first,
//     in which an na starts the extremum again;
//   * a call whose length is not longer and that has no cache reads the whole
//     window the same way.
//
// Where no na is in play the answer is the plain extremum of the last n bars.
// The bars where it is not, with the high's na on bar 6 and the low's on
// bar 8:
//
//   bar 6, highest, n 3   the high is na: na.
//   bar 8, highest, n 4   after n 1. The added bars 5, 6, 7 are read: 23.75,
//                         the na drops it, 24.5 starts again. Bar 8's own
//                         24.5 is not strictly higher: 24.5, from bar 7.
//   bar 8, lowest, n 4    the low is na: na.
//   bar 9, lowest, n 2    no cache after the na: bars 8 and 9 are read, the
//                         na first, then 22.5.
//   bar 10, highest, n 6  after n 2. The added bars 5, 6, 7, 8: the na of
//                         bar 6 drops the cache, 24.5 and 24.5 follow; bar
//                         10's own 26.0 is higher: 26.0.
//   bar 10, lowest, n 6   after n 2. The added bars 5, 6, 7, 8 are read:
//                         22.5, 21.5, 23.5 and then the na of bar 8, which
//                         drops the cache. Without one the call answers its
//                         own low, 25.0: neither the 21.5 of bar 6, the plain
//                         minimum of the six bars, nor the 22.5 of bar 9.
//   bar 12, lowest, n 5   after n 3. The added bars 8 and 9: the na, then
//                         22.5, which bar 12's own 26.0 does not beat: 22.5.
struct HandExtremes {
    double highest;
    double lowest;
};
const HandExtremes kNaTapeExtremes[24] = {
    /* bar  0, n 3 */ {NA, NA},  // fewer than three bars
    /* bar  1, n 1 */ {21.75, 20.75},
    /* bar  2, n 2 */ {21.75, 20.5},
    /* bar  3, n 4 */ {23.0, 20.0},
    /* bar  4, n 2 */ {24.0, 22.0},
    /* bar  5, n 5 */ {24.0, 20.5},
    /* bar  6, n 3 */ {NA, 21.5},
    /* bar  7, n 1 */ {24.5, 23.5},
    /* bar  8, n 4 */ {24.5, NA},
    /* bar  9, n 2 */ {24.5, 22.5},
    /* bar 10, n 6 */ {26.0, 25.0},
    /* bar 11, n 3 */ {26.5, 22.5},
    /* bar 12, n 5 */ {27.0, 22.5},
    /* bar 13, n 2 */ {28.0, 26.0},
    /* bar 14, n 4 */ {28.0, 25.5},
    /* bar 15, n 1 */ {26.0, 25.0},
    /* bar 16, n 3 */ {27.0, 24.5},
    /* bar 17, n 7 */ {28.0, 24.0},
    /* bar 18, n 2 */ {27.5, 24.0},
    /* bar 19, n 5 */ {27.5, 24.0},
    /* bar 20, n 3 */ {27.5, 26.25},
    /* bar 21, n 4 */ {28.5, 26.25},
    /* bar 22, n 2 */ {29.0, 27.5},
    /* bar 23, n 6 */ {30.0, 26.25},
};

// The extremum of the values that are not na among the last n bars: what a
// window that skipped its na members would answer. na while fewer than n
// bars exist or when every member is na.
double skipping_extreme(const std::vector<double>& values, std::size_t i, std::size_t n,
                        bool want_max) {
    if (i + 1 < n) return NA;
    double best = NA;
    for (std::size_t j = i + 1 - n; j <= i; ++j) {
        if (std::isnan(values[j])) continue;
        if (std::isnan(best) || (want_max ? values[j] > best : values[j] < best)) {
            best = values[j];
        }
    }
    return best;
}

// --- one description per function: its two classes, its inputs, its reference ---

struct Stoch {
    static constexpr const char* name = "stoch";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesStoch;
    using Fixed = ta::Stoch;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], t.high[i], t.low[i], n)
                     : s.compute(t.source[i], t.high[i], t.low[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) {
        return f.compute(t.source[i], t.high[i], t.low[i]);
    }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_stoch(t, i, n);
    }
};

struct Wpr {
    static constexpr const char* name = "wpr";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesWpr;
    using Fixed = ta::WPR;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], t.high[i], t.low[i], n)
                     : s.compute(t.source[i], t.high[i], t.low[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) {
        return f.compute(t.source[i], t.high[i], t.low[i]);
    }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_wpr(t, i, n);
    }
};

// The fixed-length path of ta.cci is the Pine call's own type. PineCci is
// ta::CCI (Kernel) except on a window whose mean deviation is zero up to
// rounding, where it answers 0.
struct Cci {
    static constexpr const char* name = "cci";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesCci;
    using Fixed = pineforge::source::PineCci;
    using Kernel = ta::CCI;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double kernel(Kernel& k, const Tapes& t, std::size_t i) {
        return k.compute(t.source[i]);
    }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_cci(t.source, i, n);
    }
};

// The fixed-length path of ta.mfi is the Pine call's own type. PineMfi is
// ta::MFI (Kernel) except on the call that completes the first `length`
// calls, where ta::MFI is still na and PineMfi has TradingView's first value.
// expectation corrected: na -> PineMfi's first value on the six rows where
// the call completes the first `length` calls (constant: bar length - 1 of
// each of the five lengths; changing: bar 3, length 4), because the series
// class is measured against the Pine call's fixed-length type, not ta::MFI.
struct Mfi {
    static constexpr const char* name = "mfi";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesMfi;
    using Fixed = pineforge::source::PineMfi;
    using Kernel = ta::MFI;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], t.volume[i], n)
                     : s.compute(t.source[i], t.volume[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) {
        return f.compute(t.source[i], t.volume[i]);
    }
    static double kernel(Kernel& k, const Tapes& t, std::size_t i) {
        return k.compute(t.source[i], t.volume[i]);
    }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_mfi(t.source, t.volume, i, n);
    }
};

struct Mom {
    static constexpr const char* name = "mom";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesMom;
    using Fixed = ta::Mom;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_mom(t.source, i, n);
    }
};

struct Roc {
    static constexpr const char* name = "roc";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesRoc;
    using Fixed = ta::ROC;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_roc(t.source, i, n);
    }
};

struct Cmo {
    static constexpr const char* name = "cmo";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesCmo;
    using Fixed = ta::CMO;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_cmo(t.source, i, n);
    }
};

struct Cog {
    static constexpr const char* name = "cog";
    static constexpr bool truth = false;
    using Series = pineforge::source::SeriesCog;
    using Fixed = ta::COG;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_cog(t.source, i, n);
    }
};

struct Rising {
    static constexpr const char* name = "rising";
    static constexpr bool truth = true;
    using Series = pineforge::source::SeriesRising;
    using Fixed = ta::Rising;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_rising(t.source, i, n);
    }
};

struct Falling {
    static constexpr const char* name = "falling";
    static constexpr bool truth = true;
    using Series = pineforge::source::SeriesFalling;
    using Fixed = ta::Falling;
    static double series(Series& s, const Tapes& t, std::size_t i, double n, bool again) {
        return again ? s.recompute(t.source[i], n) : s.compute(t.source[i], n);
    }
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
    static double direct(const Tapes& t, std::size_t i, std::size_t n) {
        return direct_falling(t.source, i, n);
    }
};

#if defined(PINEFORGE_HAS_PINE_POSITIONAL_LAG)
// Where the tree has the Pine-layer fixed-length classes of ta.mom and
// ta.roc, they are the fixed-length path of the two functions.
struct PineLayerMom : Mom {
    using Fixed = pineforge::source::PineMom;
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
};

struct PineLayerRoc : Roc {
    using Fixed = pineforge::source::PineRoc;
    static double fixed(Fixed& f, const Tapes& t, std::size_t i) { return f.compute(t.source[i]); }
};
#endif

// Target 1: a constant per-call length against the fixed-length class.
// `target` names the rows of a second tape of one function.
template <class F>
void constant_length(const Tapes& t, const char* target = "constant") {
    const std::size_t before = g_rows;
    std::size_t lengths = 0;
    std::size_t values = 0;
    std::size_t ones = 0;
    std::size_t zeros = 0;
    for (int length : {1, 2, 3, 5, 8}) {
        typename F::Series series;
        typename F::Fixed fixed(length);
        for (std::size_t i = 0; i < t.bars(); ++i) {
            const double expected = F::fixed(fixed, t, i);
            if (!std::isnan(expected)) ++values;
            if (expected == 1.0) ++ones;
            if (expected == 0.0) ++zeros;
            check(F::name, target, i, length, F::series(series, t, i, length, false), expected);
        }
        ++lengths;
    }
    assert(lengths == 5 && g_rows - before == lengths * t.bars());
    // The fixed-length class answered with values, not only with na.
    assert(values > 0);
    if (F::truth) assert(ones > 0 && zeros > 0);
}

// The rows of constant_length on which a function's Pine-layer type and its
// kernel class differ: the case the Pine-layer type exists for. A tape with
// fewer such rows than `minimum` does not hold that case, and its
// constant-length rows would compare nothing the two classes do not share.
template <class F>
void pine_type_rows(const Tapes& t, const char* tape, std::size_t minimum) {
    std::size_t rows = 0;
    for (int length : {1, 2, 3, 5, 8}) {
        typename F::Fixed pine(length);
        typename F::Kernel kernel(length);
        for (std::size_t i = 0; i < t.bars(); ++i) {
            if (!same_bits(F::fixed(pine, t, i), F::kernel(kernel, t, i))) ++rows;
        }
    }
    if (rows >= minimum) return;
    ++g_failures;
    std::fprintf(stderr,
                 "FAIL %s %s NOT COMPARED: the Pine-layer type and the kernel class differ on "
                 "%zu rows, at least %zu expected\n",
                 F::name, tape, rows, minimum);
}

// Target 2: a length that changes per bar against the direct recomputation.
// `target` names the rows of a second tape of one function.
template <class F>
void changing_length(const Tapes& t, const char* target = "changing") {
    assert(kChangingLengths.size() == t.bars());
    const std::size_t before = g_rows;
    std::size_t values = 0;
    std::size_t ones = 0;
    typename F::Series series;
    for (std::size_t i = 0; i < t.bars(); ++i) {
        const double length = kChangingLengths[i];
        const double expected = F::direct(t, i, static_cast<std::size_t>(length));
        if (!std::isnan(expected)) ++values;
        if (expected == 1.0) ++ones;
        check(F::name, target, i, length, F::series(series, t, i, length, false), expected);
    }
    assert(g_rows - before == t.bars());
    assert(values > 0);
    if (F::truth) assert(ones > 0);
}

// Target 2 for ta.stoch and ta.wpr on highs and lows that hold an na: a
// length that changes per bar against the extremes written by hand
// (kNaTapeExtremes), each function's own formula applied to them here.
// `t` is na_tape(), the tape the table was worked out on.
void changing_length_na_extremes(const Tapes& t) {
    assert(kChangingLengths.size() == t.bars());
    assert(sizeof kNaTapeExtremes / sizeof kNaTapeExtremes[0] == t.bars());
    const std::size_t before = g_rows;
    pineforge::source::SeriesStoch stoch;
    pineforge::source::SeriesWpr wpr;
    std::size_t values = 0;
    std::size_t decided = 0;
    for (std::size_t i = 0; i < t.bars(); ++i) {
        const double length = kChangingLengths[i];
        const double hi = kNaTapeExtremes[i].highest;
        const double lo = kNaTapeExtremes[i].lowest;
        const bool extremes = !std::isnan(hi) && !std::isnan(lo) && hi != lo;
        // ta.stoch: 100 * (source - lowest) / (highest - lowest), na on an na
        // source. ta.wpr: (highest - close) / (highest - lowest) * -100.
        const double source = t.source[i];
        const double expected_stoch =
            extremes && !std::isnan(source) ? 100.0 * (source - lo) / (hi - lo) : NA;
        const double expected_wpr = extremes ? (hi - source) / (hi - lo) * -100.0 : NA;
        if (!std::isnan(expected_stoch)) ++values;
        check("stoch", "changing-na-extremes", i, length,
              stoch.compute(source, t.high[i], t.low[i], length), expected_stoch);
        check("wpr", "changing-na-extremes", i, length,
              wpr.compute(source, t.high[i], t.low[i], length), expected_wpr);
        // A bar decides something when a window that skipped its na members
        // would read another extreme than the one written by hand.
        const std::size_t n = static_cast<std::size_t>(length);
        if (!same_bits(hi, skipping_extreme(t.high, i, n, true))) ++decided;
        if (!same_bits(lo, skipping_extreme(t.low, i, n, false))) ++decided;
    }
    assert(g_rows - before == 2 * t.bars());
    assert(values > 0);
    // Bar 6 for the highest, bars 8 and 10 for the lowest.
    if (decided >= 3) return;
    ++g_failures;
    std::fprintf(stderr,
                 "FAIL stoch/wpr changing-na-extremes NOT COMPARED: the hand-written extremes "
                 "differ from the na-skipping extremum on %zu rows, at least 3 expected\n",
                 decided);
}

// The length check is SeriesSma's, names this function, and records nothing.
template <class F>
void refused_length(const Tapes& t) {
    using pineforge::RunFailureCode;
    const pineforge::detail::TaLookbackScope tv_cap(15000);
    struct Invalid {
        double length;
        RunFailureCode code;
        const char* text;
        const char* rule;
    };
    const Invalid rows[] = {
        {0, RunFailureCode::pine_invalid_argument, "It must be > 0.", "must_be_positive"},
        {-3, RunFailureCode::pine_invalid_argument, "(-3)", "must_be_positive"},
        {0.9, RunFailureCode::pine_invalid_argument, "(0)", "must_be_positive"},
        {NA, RunFailureCode::pine_invalid_argument, "It must not be na", "must_not_be_na"},
        {std::numeric_limits<double>::infinity(), RunFailureCode::pine_invalid_argument,
         "It must not be na", "must_not_be_na"},
        {-std::numeric_limits<double>::infinity(), RunFailureCode::pine_invalid_argument,
         "It must not be na", "must_not_be_na"},
        {std::numeric_limits<double>::max(), RunFailureCode::pine_runtime_limit,
         "The historical buffer's limit is 15000 bars.", "max_bars_back"},
        {-std::numeric_limits<double>::max(), RunFailureCode::pine_invalid_argument,
         "It must be > 0.", "must_be_positive"},
        {15002, RunFailureCode::pine_runtime_limit, "(15001 bars back)", "max_bars_back"},
    };
    const std::size_t half = t.bars() / 2;
    assert(half >= 2);
    typename F::Series clean;
    typename F::Series tested;
    for (std::size_t i = 0; i < half; ++i) {
        const double length = kChangingLengths[i];
        check(F::name, "before-refusal", i, length, F::series(tested, t, i, length, false),
              F::series(clean, t, i, length, false));
    }
    std::size_t refused = 0;
    for (const auto& row : rows) {
        for (bool again : {false, true}) {
            bool caught = false;
            try {
                (void)F::series(tested, t, half, row.length, again);
            } catch (const pineforge::coded<std::runtime_error>& error) {
                caught = true;
                assert(error.run_failure_code() == row.code);
                const std::string what = error.what();
                assert(what.find(row.text) != std::string::npos);
                const auto& args = error.run_failure().args;
                assert(args && args->find(row.rule) != std::string::npos);
                if (row.code == RunFailureCode::pine_invalid_argument) {
                    assert(what.find(std::string("'") + F::name + "' function") !=
                           std::string::npos);
                    assert(what.find("'length' argument") != std::string::npos);
                }
            }
            assert(caught);
            ++refused;
        }
    }
    assert(refused == 18);
    // The refused calls recorded nothing: replacing the last accepted call
    // and every later bar read as on the instance that saw no refusal.
    const double last = kChangingLengths[half - 1];
    check(F::name, "after-refusal", half - 1, last, F::series(tested, t, half - 1, last, true),
          F::series(clean, t, half - 1, last, true));
    for (std::size_t i = half; i < t.bars(); ++i) {
        const double length = kChangingLengths[i];
        check(F::name, "after-refusal", i, length, F::series(tested, t, i, length, false),
              F::series(clean, t, i, length, false));
    }
}

// recompute() replaces the call made last, whatever its inputs and length.
template <class F>
void replaced_call(const Tapes& t) {
    const std::size_t before = g_rows;
    typename F::Series plain;
    typename F::Series replaced;
    for (std::size_t i = 0; i < t.bars(); ++i) {
        const double length = kChangingLengths[i];
        const double expected = F::series(plain, t, i, length, false);
        const std::size_t other = (i * 7 + 5) % t.bars();
        const std::size_t third = (other + 3) % t.bars();
        (void)F::series(replaced, t, other, kChangingLengths[other], false);
        (void)F::series(replaced, t, third, kChangingLengths[(i + 1) % t.bars()], true);
        check(F::name, "replaced", i, length, F::series(replaced, t, i, length, true), expected);
    }
    assert(g_rows - before == t.bars());
    // A recompute() before any compute() is the first compute().
    typename F::Series first;
    typename F::Series started;
    check(F::name, "first-recompute", 0, 1, F::series(started, t, 0, 1, true),
          F::series(first, t, 0, 1, false));
    check(F::name, "first-recompute", 1, 1, F::series(started, t, 1, 1, false),
          F::series(first, t, 1, 1, false));
}

// A lookback cap bounds the history and changes no admitted answer.
template <class F>
void capped_lookback(const Tapes& t) {
    using pineforge::RunFailureCode;
    const std::size_t before = g_rows;
    typename F::Series whole;   // built without a cap: keeps every value
    const pineforge::detail::TaLookbackScope cap(4);
    typename F::Series bounded;
    for (std::size_t i = 0; i < t.bars(); ++i) {
        // 1 .. 5: every length a cap of 4 admits.
        const double length = static_cast<double>(1 + (i * 3 + 1) % 5);
        const double expected = F::series(whole, t, i, length, false);
        const std::size_t other = (i * 7 + 5) % t.bars();
        (void)F::series(bounded, t, other, static_cast<double>(1 + i % 5), false);
        check(F::name, "capped", i, length, F::series(bounded, t, i, length, true), expected);
    }
    assert(g_rows - before == t.bars());
    bool caught = false;
    try {
        (void)F::series(bounded, t, 0, 6, false);
    } catch (const pineforge::coded<std::runtime_error>& error) {
        caught = true;
        assert(error.run_failure_code() == RunFailureCode::pine_runtime_limit);
        const std::string what = error.what();
        assert(what.find("(5 bars back)") != std::string::npos);
        assert(what.find("The historical buffer's limit is 4 bars.") != std::string::npos);
    }
    assert(caught);
}

// A copy is a checkpoint: writing to one of the two never reaches the other.
template <class F>
void copied_state(const Tapes& t) {
    const std::size_t half = t.bars() / 2;
    typename F::Series plain;
    typename F::Series original;
    for (std::size_t i = 0; i < half; ++i) {
        const double length = kChangingLengths[i];
        (void)F::series(plain, t, i, length, false);
        (void)F::series(original, t, i, length, false);
    }
    typename F::Series stray = original;
    (void)F::series(stray, t, 0, kChangingLengths[0], false);
    (void)F::series(stray, t, 1, kChangingLengths[1], true);
    typename F::Series second = original;
    for (std::size_t i = half; i < t.bars(); ++i) {
        const double length = kChangingLengths[i];
        const double expected = F::series(plain, t, i, length, false);
        check(F::name, "copied-original", i, length, F::series(original, t, i, length, false),
              expected);
        check(F::name, "copied-second", i, length, F::series(second, t, i, length, false),
              expected);
    }
}

template <class F>
void shared_behaviour(const Tapes& with_na, const Tapes& generated) {
    refused_length<F>(with_na);
    replaced_call<F>(with_na);
    capped_lookback<F>(generated);
    copied_state<F>(with_na);
}

}  // namespace

int main() {
    const Tapes with_na = na_tape();
    const Tapes clean_extremes = clean_extremes_tape();
    const Tapes leading_na = leading_na_tape();
    const Tapes generated = generated_tape();
    const Tapes zero_first_flow = zero_first_flow_tape();
    const Tapes flat_window = flat_window_tape();

    // Target 1. ta::Mom and ta::ROC keep a window of present samples, so
    // they are the fixed-length path of SeriesMom and SeriesRoc only on a
    // source with no na after its first value.
    constant_length<Stoch>(with_na);
    constant_length<Wpr>(with_na);
    constant_length<Cci>(with_na);
    constant_length<Mfi>(with_na);
    constant_length<Cmo>(with_na);
    constant_length<Cog>(with_na);
    constant_length<Rising>(with_na);
    constant_length<Falling>(with_na);
    constant_length<Mom>(leading_na);
    constant_length<Roc>(leading_na);
#if defined(PINEFORGE_HAS_PINE_POSITIONAL_LAG)
    constant_length<PineLayerMom>(with_na);
    constant_length<PineLayerRoc>(with_na);
#endif

    // Target 1 on the case the Pine-layer types of ta.mfi and ta.cci exist
    // for. The na tape has one first value per length (5). The tape whose
    // first flow is 0 has the two first values of 100; its three longer
    // lengths have an na first value, as ta::MFI has. The flat tape has
    // twelve windows of three values of 0.1 (length 3: bars 2 to 7, 11, 13
    // to 15, 22 and 23).
    pine_type_rows<Mfi>(with_na, "na-tape", 5);
    pine_type_rows<Mfi>(zero_first_flow, "zero-first-flow", 2);
    pine_type_rows<Cci>(flat_window, "flat-window", 12);
    constant_length<Mfi>(zero_first_flow, "constant-zero-first-flow");
    constant_length<Cci>(flat_window, "constant-flat-window");

    // Target 2. The highs and lows hold no na here. The window extremes of a
    // changing length are SeriesWindowExtreme's, which answers from its cached
    // extremum when a window that has just grown holds an na; without an na
    // it is the plain extremum of the last `length` bars, the reference here.
    changing_length<Stoch>(clean_extremes);
    changing_length<Wpr>(clean_extremes);
    // The same two functions on the highs and lows that hold an na, against
    // extremes written by hand from that rule.
    changing_length_na_extremes(with_na);
    changing_length<Cci>(with_na);
    changing_length<Cci>(flat_window, "changing-flat-window");
    changing_length<Mfi>(with_na);
    changing_length<Mom>(with_na);
    changing_length<Roc>(with_na);
    changing_length<Cmo>(with_na);
    changing_length<Cog>(with_na);
    changing_length<Rising>(with_na);
    changing_length<Falling>(with_na);

    shared_behaviour<Stoch>(with_na, generated);
    shared_behaviour<Wpr>(with_na, generated);
    shared_behaviour<Cci>(with_na, generated);
    shared_behaviour<Mfi>(with_na, generated);
    shared_behaviour<Mom>(with_na, generated);
    shared_behaviour<Roc>(with_na, generated);
    shared_behaviour<Cmo>(with_na, generated);
    shared_behaviour<Cog>(with_na, generated);
    shared_behaviour<Rising>(with_na, generated);
    shared_behaviour<Falling>(with_na, generated);

    assert(g_rows > 0);
    if (std::printf("test_series_osc_length: %zu rows compared, %d differ\n", g_rows,
                    g_failures) < 0) {
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
