// Fixed-length ta.lowest (source::PineFixedLowest): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, "Invalid value of the 'length'
// argument (0) in the 'lowest' function. It must be > 0.", the one a series
// length of the same value raises (source::SeriesLowest), and a valid length
// computes ta::Lowest's values. ta::Lowest itself takes a length of 0 and
// answers na on every bar. Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.lowest call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedLowest;
#else
using Fixed = ta::Lowest;
#endif

// TradingView names the argument and the function as the call does.
const Site kSite{"lowest", "length", "'lowest'", "length", "> 0"};

// ta.lowest(length), which reads low, and ta.lowest(low, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) { return call.compute(bar.low); }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) { return call.recompute(bar.low); }
};

// The same call with a series length.
struct Twin {
    static constexpr bool present = true;
    static void call(double length, const Bar& bar) {
        pineforge::source::SeriesLowest series;
        series.compute(bar.low, length);
    }
};

}  // namespace

int main() {
    // Five invented bars, open = high = low = close: 5, 7, 4, 8, 6.
    const std::vector<Bar> bars = {
        {5, 5, 5, 1}, {7, 7, 7, 1}, {4, 4, 4, 1}, {8, 8, 8, 1}, {6, 6, 6, 1}};
    // ta.lowest(low, 1): the bar's own low.
    return run_single_length<Fixed, ta::Lowest, Feed, Twin>(
        "checked_fixed_lowest", kSite, bars[0], bars[0], 1, bars, {5.0, 7.0, 4.0, 8.0, 6.0});
}
