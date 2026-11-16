// test_ta_sma_price_tape -- on a price series ta.sma is the correctly rounded
// exact sum of its window divided by the length. The source that entered the
// window is what leaves it `length` bars later, so the compensation the
// running sum applied on the way in is not left behind in the sum.
//
// Pinned on a full-precision TradingView tape taken 2026-11-16: a deep
// backtest of BINANCE:BTCUSDT 60 over 2024-01-01..2024-03-01 UTC (script
// sha256 20aa1262e9b81ca07a94f168b6afbc2ab796aeb6626e6f2872b00438437d822c,
// trade list sha256
// 368dd136315d3660188468c71eae8ca747ba6b46105522ae5b87c36c4dc0982b). From the
// first bar of the range the script computes
//     average = ta.sma(inWindow ? close : na, 3)
// and each entry's order comment carries the average of its own bar and of
// the bar before as `whole:fraction`, the value being whole + fraction / 2^52
// with both parts printed as integers, so the text holds every bit of the
// double. Entries are placed on bars 2, 5, 8, ...: the first twenty give the
// na of bar 1 and the 39 averages of bars 2..59 below (bars 3, 6, 9, ... are
// not exported).
//
// What the rows pin:
//   * every exported average is fl(fl(exact window sum) / 3): the sum of the
//     three closes rounded ONCE, to nearest-even, then divided. This file
//     checks that against an error-free sum that does not use the engine, so
//     a mistyped row cannot pass or fail as an engine result;
//   * the rounding of the SUM is what TradingView divides: bar 4 exports
//     42437:870695927939072, one unit in the last place below the correctly
//     rounded mean of 42437.19333... (42437:870695927971840), because the
//     exact sum of its window is a tie that rounds down;
//   * nothing is carried from the sources that left: a ring that subtracts
//     the compensated addend x - c instead of the source x leaves c in the
//     running sum for good and reads one unit in the last place high on bar 7
//     already (42374:1741391855943680).

#include <pineforge/ta.hpp>
#include <pineforge/na.hpp>

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

// The first 60 hourly closes of the range (bar 0 = 2024-01-01 00:00 UTC), as
// the feed prints them.
const double kClose[] = {
    42475.23, 42613.56, 42581.1,  42330.49, 42399.99, 42234.01, 42396.69, 42492.46, 42549.99, 42649.69,
    42691.1,  42690.2,  42648.38, 42715.54, 42612.24, 42783.05, 42725.15, 42812.7,  43111.1,  43517.99,
    43687.86, 43619.21, 43529.93, 44179.55, 45056,    44855.96, 45378.96, 45402.38, 45179.8,  45179.48,
    45168.75, 45455.53, 45759.3,  45673.48, 45584.99, 45433.99, 45301.99, 45637.93, 45258.73, 45184.82,
    45357.84, 44966.15, 45154.69, 45039.49, 44788.28, 45119.74, 44981.73, 44946.91, 45135.61, 45317.58,
    45274.57, 45262.69, 45183.45, 45172.13, 45326.93, 45011.77, 45200,    45344.67, 45151.91, 43702.88,
};
constexpr int kBars = static_cast<int>(sizeof(kClose) / sizeof(kClose[0]));

struct Average {
    int bar;
    long long whole;
    long long fraction;   // the value is whole + fraction / 2^52
};

// TradingView's ta.sma(close, 3) on bars 2..59, copied from the order
// comments of the tape's first twenty entries.
const Average kTape[] = {
    {2, 42556, 2837267765231616LL},  {4, 42437, 870695927939072LL},   {5, 42321, 2236787814924288LL},
    {7, 42374, 1741391855910912LL},  {8, 42479, 3212567734190080LL},  {10, 42630, 1170935903125504LL},
    {11, 42676, 4488587628609536LL}, {13, 42684, 3182543736668160LL}, {14, 42658, 3242591731712000LL},
    {16, 42706, 3662927696920576LL}, {17, 42773, 2852279763992576LL}, {19, 43147, 1185947901853696LL},
    {20, 43438, 4428539633565696LL}, {22, 43612, 1501199875801088LL}, {23, 43776, 1035827914309632LL},
    {25, 44697, 765611936677888LL},  {26, 45096, 4383503637282816LL}, {28, 45320, 1711367858421760LL},
    {29, 45253, 3993191669596160LL}, {31, 45267, 4143311657205760LL}, {32, 45461, 870695927971840LL},
    {34, 45672, 2657123780165632LL}, {35, 45564, 690551942840320LL},  {37, 45457, 4368491638554624LL},
    {38, 45399, 2476979795034112LL}, {40, 45267, 585467951579136LL},  {41, 45169, 2717171775176704LL},
    {43, 45053, 1996595834814464LL}, {44, 44994, 690551942840320LL},  {46, 44963, 1125899906842624LL},
    {47, 45016, 570455952818176LL},  {49, 45133, 1651319863377920LL}, {50, 45242, 2642111781404672LL},
    {52, 45240, 1065851911798784LL}, {53, 45206, 405323966447616LL},  {55, 45170, 1245995896897536LL},
    {56, 45179, 2552039788871680LL}, {58, 45232, 870695927971840LL},  {59, 44733, 690551942840320LL},
};

// Both steps are exact: fraction < 2^52 converts exactly, and the sum is the
// exported double itself.
double exported(const Average& row) {
    return static_cast<double>(row.whole)
        + std::ldexp(static_cast<double>(row.fraction), -52);
}

// The tape's own encoding of a double of magnitude >= 1.
void print_dyadic(const char* label, double value) {
    const double whole = std::floor(value);
    std::printf("%s %.0f:%.0f", label, whole, std::ldexp(value - whole, 52));
}

// a + b as its rounded sum and the exact rounding error (Knuth's TwoSum).
void two_sum(double a, double b, double& sum, double& error) {
    sum = a + b;
    const double b_seen = sum - a;
    const double a_seen = sum - b_seen;
    error = (a - a_seen) + (b - b_seen);
}

// The exact sum of three doubles, rounded once. On these rows both rounding
// errors are small whole multiples of the closes' last place, so their sum is
// exact and adding it to the rounded partial sum rounds the exact total.
double exact_sum3(double oldest, double middle, double newest) {
    double first = 0.0, first_error = 0.0, total = 0.0, total_error = 0.0;
    two_sum(oldest, middle, first, first_error);
    two_sum(first, newest, total, total_error);
    return total + (first_error + total_error);
}

void test_exported_averages() {
    ta::SMA sma(3);
    double value[kBars];
    for (int bar = 0; bar < kBars; ++bar) value[bar] = sma.compute(kClose[bar]);

    // The tape's first entry (bar 2) exports na for the average of bar 1.
    CHECK_TRUE(is_na(value[0]) && is_na(value[1]), "bars 0 and 1 are na");

    int off_rule = 0, off_tape = 0;
    for (const Average& row : kTape) {
        const double tv = exported(row);
        const double rule =
            exact_sum3(kClose[row.bar - 2], kClose[row.bar - 1], kClose[row.bar]) / 3;
        if (rule != tv) {
            if (++off_rule <= 5) {
                std::printf("  bar %d:", row.bar);
                print_dyadic(" exactly rounded sum / 3", rule);
                print_dyadic(", TradingView", tv);
                std::printf("\n");
            }
        }
        if (value[row.bar] != tv) {
            if (++off_tape <= 5) {
                std::printf("  bar %d:", row.bar);
                print_dyadic(" ta.sma", value[row.bar]);
                print_dyadic(", TradingView", tv);
                std::printf("\n");
            }
        }
    }
    const int rows = static_cast<int>(sizeof(kTape) / sizeof(kTape[0]));
    std::printf("  %d exported averages: %d off the exactly rounded sum / 3, %d off ta.sma\n",
                rows, off_rule, off_tape);
    CHECK_TRUE(rows == 39, "the 39 averages of bars 2..59");
    CHECK_TRUE(off_rule == 0, "every exported average is the exactly rounded window sum / 3");
    CHECK_TRUE(off_tape == 0, "ta.sma reproduces every exported average bit for bit");
}

// A double in [2^15, 2^16) as a whole number of its last place, 2^-37.
long long last_places(const Average& row) {
    return row.whole * (1LL << 37) + row.fraction / (1LL << 15);
}

// Bar 4 is the row that separates the sum's rounding from the mean's: the
// exact sum of its window is a tie in the sum's last place and rounds down.
// Its closes are 4258110 + 4233049 + 4239999 = 12731158 cents, so the exact
// mean is 12731158 / 300, which is 12731158 * 2^35 / 75 last places: m is its
// correct rounding exactly when |75 m - 12731158 * 2^35| <= 37, in integers.
void test_sum_rounding_is_divided() {
    const Average tape{4, 42437, 870695927939072LL};
    const Average mean{4, 42437, 870695927971840LL};
    const long long exact75 = 12731158LL * (1LL << 35);
    const long long mean_error = 75 * last_places(mean) - exact75;
    const long long tape_error = 75 * last_places(tape) - exact75;
    CHECK_TRUE(mean_error >= -37 && mean_error <= 37,
               "bar 4: 42437:870695927971840 is the correctly rounded mean");
    CHECK_TRUE(last_places(mean) - last_places(tape) == 1 && tape_error < -37,
               "bar 4: the exported average is one unit in the last place below it");
    CHECK_TRUE(exact_sum3(kClose[2], kClose[3], kClose[4]) / 3 == exported(tape),
               "bar 4: the exactly rounded sum divided by 3 is the exported average");
}

// Bar 4 is not alone: against the correctly rounded mean of the three closes
// (their cents K over 300, which is K * 2^35 / 75 last places) every exported
// average is the same double or its neighbour, and both neighbours occur.
void test_rows_beside_the_rounded_mean() {
    int below = 0, above = 0, further = 0;
    for (const Average& row : kTape) {
        long long cents = 0;
        for (int bar = row.bar - 2; bar <= row.bar; ++bar)
            cents += std::llround(kClose[bar] * 100.0);
        const long long rounded_mean = (cents * (1LL << 35) + 37) / 75;
        const long long delta = last_places(row) - rounded_mean;
        if (delta == -1) ++below;
        else if (delta == 1) ++above;
        else if (delta != 0) ++further;
    }
    std::printf("  against the correctly rounded mean: %d rows one unit below, %d one unit above\n",
                below, above);
    CHECK_TRUE(further == 0, "no exported average is further than one unit from the rounded mean");
    CHECK_TRUE(below > 0 && above > 0, "exported averages sit on both sides of the rounded mean");
}

}  // namespace

int main() {
    test_exported_averages();
    test_sum_rounding_is_divided();
    test_rows_beside_the_rounded_mean();
    std::printf("test_ta_sma_price_tape: passed=%d failed=%d\n", tests_passed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
