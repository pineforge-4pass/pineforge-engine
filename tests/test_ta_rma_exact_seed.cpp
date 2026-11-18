// test_ta_rma_exact_seed -- ta.rma seeds with the exact-sum average, as
// TradingView does; the recursion after the seed is unchanged.
//
// TradingView's own doubles, read for ta.rma(close, 3) on BINANCE:ETHUSDT.P
// 15m over 40 bars from 2026-03-23 09:00 UTC (the close of each bar and the
// value ta.rma showed on it, both as IEEE-754 bit patterns, in the two tables
// below):
//   * the seed on bar 2 is the exact sum of the first three closes, rounded
//     once, divided by 3: 0x409fe269d0369d04. The running sum this engine used
//     before gives 0x409fe269d0369d03, one unit in the last place lower;
//   * bar 3 follows from that seed by (src + (length - 1) * previous) / length
//     and reads 0x409fe51d950c83fc; from the lower seed it read ...83fb;
//   * from bar 4 on the series is that recursion bit for bit (bar 5 reads
//     0x409fef85c0898b71, bar 39 reads 0x40a0d554b9d874cd).
//
// The seed is the value ta.sma gives for the same window: the rows at the end
// compare the first value of ta.rma(n) with the first value of ta.sma(n) on the
// captured closes and on windows whose running sum loses a term.

#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pineforge;
using pineforge::ta::ATR;
using pineforge::ta::DMI;
using pineforge::ta::DMIResult;
using pineforge::ta::RMA;
using pineforge::ta::RSI;
using pineforge::ta::SMA;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (false)

double from_bits(std::uint64_t bits) {
    double out = 0.0;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

std::uint64_t to_bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// Bit-exact, with na equal to na only.
bool same(double a, double b) {
    if (is_na(a) || is_na(b)) return is_na(a) && is_na(b);
    return to_bits(a) == to_bits(b);
}

// The close of each of the 40 bars.
constexpr std::uint64_t kCloseBits[40] = {
    0x409fe70000000000ULL, 0x409feaa3d70a3d71ULL, 0x409fd5999999999aULL, 0x409fea851eb851ecULL,
    0x40a002051eb851ecULL, 0x409fefb851eb851fULL, 0x409fee51eb851eb8ULL, 0x409fff51eb851eb8ULL,
    0x40a0f1d1eb851eb8ULL, 0x40a0ed7ae147ae14ULL, 0x40a0d49eb851eb85ULL, 0x40a0abc7ae147ae1ULL,
    0x40a0880000000000ULL, 0x40a09875c28f5c29ULL, 0x40a0c05c28f5c28fULL, 0x40a0be0000000000ULL,
    0x40a0c2199999999aULL, 0x40a0ca4ccccccccdULL, 0x40a0efae147ae148ULL, 0x40a1117ae147ae14ULL,
    0x40a108c28f5c28f6ULL, 0x40a106570a3d70a4ULL, 0x40a0fc8f5c28f5c3ULL, 0x40a108bd70a3d70aULL,
    0x40a1038f5c28f5c3ULL, 0x40a0e723d70a3d71ULL, 0x40a0ca28f5c28f5cULL, 0x40a09f199999999aULL,
    0x40a0b3a3d70a3d71ULL, 0x40a08ee147ae147bULL, 0x40a0a4dc28f5c28fULL, 0x40a0ada3d70a3d71ULL,
    0x40a0ace147ae147bULL, 0x40a0a48000000000ULL, 0x40a0c40f5c28f5c3ULL, 0x40a0cee666666666ULL,
    0x40a0ca8000000000ULL, 0x40a0c25c28f5c28fULL, 0x40a0d5dc28f5c28fULL, 0x40a0ef5c28f5c28fULL,
};

// ta.rma(close, 3) on each bar; the first two bars are na (zero here).
constexpr std::uint64_t kTradingViewRmaBits[40] = {
    0x0000000000000000ULL, 0x0000000000000000ULL, 0x409fe269d0369d04ULL, 0x409fe51d950c83fcULL,
    0x409fef6c77d88e9bULL, 0x409fef85c0898b71ULL, 0x409fef1f2432bc88ULL, 0x409ff48566a387edULL,
    0x40a04cc7c60d8ce1ULL, 0x40a08258cf20ed47ULL, 0x40a09dc572314207ULL, 0x40a0a270db7cffa5ULL,
    0x40a099a092535519ULL, 0x40a0993cf7bcacc9ULL, 0x40a0a6475d7a5eb5ULL, 0x40a0ae2f93a6e9ceULL,
    0x40a0b4d2eaf7cf12ULL, 0x40a0bbfb8b9423a5ULL, 0x40a0cd370e8bb831ULL, 0x40a0e3f854ca5f7dULL,
    0x40a0f03bbda5a2a5ULL, 0x40a0f79a2c82e74fULL, 0x40a0f9413c64ec20ULL, 0x40a0fe6aa3248fc3ULL,
    0x40a100218b7b5c6dULL, 0x40a0f7ccfa005219ULL, 0x40a0e8964debbbdaULL, 0x40a0d0176725b06fULL,
    0x40a0c69b8c71df70ULL, 0x40a0b40820309bc9ULL, 0x40a0aef978725360ULL, 0x40a0ae8797fa4c10ULL,
    0x40a0adfad28b8edfULL, 0x40a0aad1e1b25f3fULL, 0x40a0b33bb52f3c15ULL, 0x40a0bc749aec4a30ULL,
    0x40a0c12311f2dc20ULL, 0x40a0c18b6ef3d39bULL, 0x40a0c8510249cdecULL, 0x40a0d554b9d874cdULL,
};

std::vector<double> closes() {
    std::vector<double> out;
    for (std::uint64_t bits : kCloseBits) out.push_back(from_bits(bits));
    return out;
}

double expected_rma(int bar) {
    return bar < 2 ? na<double>() : from_bits(kTradingViewRmaBits[bar]);
}

// ta.rma(close, 3) reads the series TradingView printed, bit for bit.
void test_the_series_TradingView_printed() {
    std::printf("test_the_series_TradingView_printed\n");
    const std::vector<double> close = closes();
    RMA rma(3);
    for (int bar = 0; bar < 40; ++bar) {
        const double got = rma.compute(close[bar]);
        if (!same(got, expected_rma(bar)))
            std::printf("  bar %d: got %016llx, TradingView %016llx\n", bar,
                        static_cast<unsigned long long>(to_bits(got)),
                        static_cast<unsigned long long>(kTradingViewRmaBits[bar]));
        CHECK(same(got, expected_rma(bar)));
    }
}

// A bar that is computed again with another source, or with none, leaves no
// trace: the values that follow are the values of a series computed once. The
// replays start on every bar of the seed and on the bars right after it.
void test_a_replayed_bar_leaves_no_trace() {
    std::printf("test_a_replayed_bar_leaves_no_trace\n");
    const std::vector<double> close = closes();
    for (int replayed = 0; replayed < 8; ++replayed) {
        for (int variant = 0; variant < 2; ++variant) {
            RMA rma(3);
            bool all_same = true;
            for (int bar = 0; bar < 40; ++bar) {
                double got = rma.compute(close[bar]);
                if (bar == replayed) {
                    const double other = variant == 0 ? close[bar] * 1.5 + 1.0 : na<double>();
                    (void)rma.recompute(other);
                    got = rma.recompute(close[bar]);
                }
                if (!same(got, expected_rma(bar))) all_same = false;
            }
            if (!all_same) std::printf("  replay on bar %d, variant %d\n", replayed, variant);
            CHECK(all_same);
        }
    }
}

// The seed is ta.sma's value for the window: the first value of ta.rma(n) is
// the first value of ta.sma(n), bit for bit.
void check_the_seed_is_the_sma(const std::vector<double>& series, int length, const char* label) {
    RMA rma(length);
    SMA sma(length);
    double first_rma = na<double>();
    double first_sma = na<double>();
    for (const double value : series) {
        const double r = rma.compute(value);
        const double s = sma.compute(value);
        if (is_na(first_rma) && !is_na(r)) first_rma = r;
        if (is_na(first_sma) && !is_na(s)) first_sma = s;
    }
    if (!same(first_rma, first_sma))
        std::printf("  %s, length %d: ta.rma %016llx, ta.sma %016llx\n", label, length,
                    static_cast<unsigned long long>(to_bits(first_rma)),
                    static_cast<unsigned long long>(to_bits(first_sma)));
    CHECK(!is_na(first_rma));
    CHECK(same(first_rma, first_sma));
}

void test_the_seed_is_the_ta_sma_value() {
    std::printf("test_the_seed_is_the_ta_sma_value\n");
    const std::vector<double> close = closes();
    for (int length : {2, 3, 5, 7, 14, 20, 39}) check_the_seed_is_the_sma(close, length, "captured closes");
    // A running sum loses the 1 here (1e16 + 1 == 1e16) and reads 0; the exact
    // sum of the window is 1.
    check_the_seed_is_the_sma({1e16, 1.0, -1e16, 5.0}, 3, "a lost term");
    check_the_seed_is_the_sma({-1e16, 1.0, 1e16, 5.0}, 3, "a lost term, reversed");
    check_the_seed_is_the_sma({1e16, 1.0, 1.0, -1e16, 5.0}, 4, "two lost terms");
    // Ten times 0.1: the exact multiple of the stored double, not the running sum.
    check_the_seed_is_the_sma(std::vector<double>(12, 0.1), 10, "a repeated value");
    check_the_seed_is_the_sma({0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}, 8, "tenths");
}

// ta.rsi, ta.atr and ta.dmi save their RMAs at the start of every bar and
// return before computing them when the bar's input is na. When such a bar is
// computed again with a value, the replay must leave each seed window as the
// bar before it left it: the values that follow are those of a series computed
// once. The replays start on every bar of the seeds and on the bars after them.
void test_a_replayed_na_bar_leaves_the_seeds_alone() {
    std::printf("test_a_replayed_na_bar_leaves_the_seeds_alone\n");
    const std::vector<double> close = closes();
    const int count = 16;
    for (int replayed = 0; replayed < 10; ++replayed) {
        {
            RSI straight(3);
            RSI replay(3);
            bool all_same = true;
            for (int bar = 0; bar < count; ++bar) {
                const double want = straight.compute(close[bar]);
                double got = 0.0;
                if (bar == replayed) {
                    (void)replay.compute(na<double>());
                    got = replay.recompute(close[bar]);
                } else {
                    got = replay.compute(close[bar]);
                }
                if (!same(got, want)) all_same = false;
            }
            if (!all_same) std::printf("  ta.rsi, na bar replayed at %d\n", replayed);
            CHECK(all_same);
        }
        {
            ATR straight(3);
            ATR replay(3);
            bool all_same = true;
            for (int bar = 0; bar < count; ++bar) {
                const double high = close[bar] * 1.001;
                const double low = close[bar] * 0.999;
                const double want = straight.compute(high, low, close[bar]);
                double got = 0.0;
                if (bar == replayed) {
                    (void)replay.compute(na<double>(), na<double>(), na<double>());
                    got = replay.recompute(high, low, close[bar]);
                } else {
                    got = replay.compute(high, low, close[bar]);
                }
                if (!same(got, want)) all_same = false;
            }
            if (!all_same) std::printf("  ta.atr, na bar replayed at %d\n", replayed);
            CHECK(all_same);
        }
        {
            DMI straight(3, 3);
            DMI replay(3, 3);
            bool all_same = true;
            for (int bar = 0; bar < count; ++bar) {
                const double high = close[bar] * 1.001;
                const double low = close[bar] * 0.999;
                const DMIResult want = straight.compute(high, low, close[bar]);
                DMIResult got{};
                if (bar == replayed) {
                    (void)replay.compute(na<double>(), na<double>(), na<double>());
                    got = replay.recompute(high, low, close[bar]);
                } else {
                    got = replay.compute(high, low, close[bar]);
                }
                if (!same(got.diplus, want.diplus) || !same(got.diminus, want.diminus)
                    || !same(got.adx, want.adx))
                    all_same = false;
            }
            if (!all_same) std::printf("  ta.dmi, na bar replayed at %d\n", replayed);
            CHECK(all_same);
        }
    }
}

// The numbers the first rows of this file give in words: the lost term is a
// real difference, so the rows above would fail on a running sum.
void test_a_running_sum_would_read_otherwise() {
    std::printf("test_a_running_sum_would_read_otherwise\n");
    RMA rma(3);
    CHECK(is_na(rma.compute(1e16)));
    CHECK(is_na(rma.compute(1.0)));
    const double seed = rma.compute(-1e16);
    CHECK(same(seed, 1.0 / 3.0));
}

}  // namespace

int main() {
    test_the_series_TradingView_printed();
    test_a_replayed_bar_leaves_no_trace();
    test_the_seed_is_the_ta_sma_value();
    test_a_replayed_na_bar_leaves_the_seeds_alone();
    test_a_running_sum_would_read_otherwise();
    std::printf("\n=== Results: %d checks, %d failures ===\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
