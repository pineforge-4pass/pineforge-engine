// The SAME TU is compiled first against the frozen base's complete header
// closure and TA sources, then against the candidate. The driver compares
// every emitted byte, including NaN and signed-zero representations.
#include <pineforge/series.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>
#include <cstdio>
#include <cstdlib>

using namespace pineforge;

static void emit(double value) {
    if (std::fwrite(&value, sizeof value, 1, stdout) != 1) std::abort();
}

static double input(int bar) {
    if (bar % 67 == 0) return na<double>();
    if (bar % 23 == 0) return -0.0;
    if (bar % 29 == 0) return 0.0;
    if (bar % 13 == 0) return 0.7000000000000001;
    if (bar % 11 == 0) return 1e-15;
    return static_cast<double>((bar * 37) % 101 - 50) / 10.0;
}

template<class T>
static void unary(T object) {
    T saved(object);
    for (int bar = 0; bar < 20003; ++bar) {
        if (bar % 7 == 0) continue;
        saved = object;
        emit(object.compute(input(bar)));
        emit(object.recompute(input(bar + 1)));
        emit(object.recompute(input(bar + 2)));
        if (bar % 19 == 0) object = saved;
    }
}

int main() {
    for (const int length : {1, 2, 5, 37}) {
        unary(ta::SMA(length)); unary(ta::WMA(length));
        unary(ta::EMA(length)); unary(ta::RMA(length));
        unary(ta::StdDev(length)); unary(ta::StdDev(length, false));
        unary(ta::Variance(length)); unary(ta::Variance(length, false));
        unary(ta::ALMA(length)); unary(ta::Median(length)); unary(ta::Mode(length));
        unary(ta::RCI(length)); unary(ta::Mom(length)); unary(ta::ROC(length));
        unary(ta::Rising(length)); unary(ta::Falling(length));
        unary(ta::CCI(length)); unary(ta::COG(length)); unary(ta::Dev(length));
        unary(ta::CMO(length));
        ta::MFI mfi(length);
        ta::VWMA vwma(length);
        ta::Correlation corr(length);
        ta::Linreg linreg(length);
        ta::PercentRank rank(length);
        ta::PercentileNearestRank nearest(length);
        ta::PercentileLinearInterpolation linear(length);
        ta::Change change(length);
        for (int bar = 0; bar < 4099; ++bar) {
            const double x = input(bar), y = input(bar + 3);
            emit(mfi.compute(x, y)); emit(mfi.recompute(y, x));
            emit(vwma.compute(x, y)); emit(vwma.recompute(y, x));
            emit(corr.compute(x, y)); emit(corr.recompute(y, x));
            emit(linreg.compute(x, 2)); emit(linreg.recompute(y, 2));
            emit(rank.compute(x)); emit(rank.recompute(y));
            emit(nearest.compute(x, 70)); emit(nearest.recompute(y, 70));
            emit(linear.compute(x, 70)); emit(linear.recompute(y, 70));
            const int dynamic = bar % 31 == 0 ? length * 3 : length;
            emit(change.compute(x, dynamic)); emit(change.recompute(y, dynamic));
        }
    }
    unary(ta::SWMA()); unary(ta::PivotHigh(3, 5)); unary(ta::PivotLow(3, 5));
    ta::ValueWhen when(7);
    for (int bar = 0; bar < 4099; ++bar) {
        emit(when.compute(bar % 3 == 0, input(bar), bar % 7));
        emit(when.recompute(bar % 5 == 0, input(bar + 1), bar % 7));
    }
    source::SeriesHighest highest;
    source::SeriesLowest lowest;
    source::SeriesHighestBars highbars;
    source::SeriesLowestBars lowbars;
    source::SeriesLowest checkpoint;
    for (int bar = 0; bar < 40007; ++bar) {
        ta::BarContextScope context(bar, 0);
        if (bar % 7 != 0 && bar % 13 != 0) continue;
        const double x = input(bar), y = input(bar + 1);
        const int length = bar % 3 == 0 ? 15001 : bar % 97 + 1;
        checkpoint = lowest;
        emit(highest.compute(x, length)); emit(highest.recompute(y, length));
        emit(lowest.compute(x, length)); emit(lowest.recompute(y, length));
        emit(highbars.compute(x, length)); emit(highbars.recompute(y, length));
        emit(lowbars.compute(x, length)); emit(lowbars.recompute(y, length));
        if (bar % 19 == 0) lowest = checkpoint;
    }
    {
        ta::BarContextScope context(100000, 0);
        emit(lowest.compute(1.0, 15001));
        emit(lowest.recompute(2.0, 15001));
    }
    for (const char* name : {"Traditional", "Fibonacci", "Woodie", "Classic", "DM", "Camarilla"}) {
        ta::PivotPointLevels pivot;
        for (int bar = 0; bar < 1027; ++bar) {
            const double x = static_cast<double>(bar % 101);
            const auto out = pivot.compute(name, bar % 7 == 0, false, x, x + 2, x - 2, x + 1);
            for (double v : out) emit(v);
            const auto again = pivot.recompute(name, bar % 11 == 0, false, x + 1, x + 3, x - 3, x);
            for (double v : again) emit(v);
            const auto free_hlc = ta::pivot_point_levels(name, x + 2, x - 2, x + 1);
            for (double v : free_hlc) emit(v);
            const auto free_ohlc = ta::pivot_point_levels(name, x, x + 2, x - 2, x + 1, x + 0.5);
            for (double v : free_ohlc) emit(v);
        }
    }
    return std::fflush(stdout) == 0 ? 0 : 1;
}
