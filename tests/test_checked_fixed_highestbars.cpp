// Fixed-length ta.highestbars (source::PineFixedHighestBars): a length of 0,
// a negative length or an na length that is a constant or an input stops the
// call's first execution with TradingView's error, "Invalid value of the
// 'length' argument (0) in the 'highestbars' function. It must be > 0.", the
// one a series length of the same value raises (source::SeriesHighestBars),
// and a valid length computes ta::HighestBars's values. ta::HighestBars itself
// takes a length of 0 and answers na on every bar.
// Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.highestbars call with a constant or input length is built
// with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedHighestBars;
#else
using Fixed = ta::HighestBars;
#endif

// TradingView names the argument and the function as the call does.
const Site kSite{"highestbars", "length", "'highestbars'", "length", "> 0"};

// ta.highestbars(length), which reads high, and ta.highestbars(high, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) { return call.compute(bar.high); }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) { return call.recompute(bar.high); }
};

// The same call with a series length.
struct Twin {
    static constexpr bool present = true;
    static void call(double length, const Bar& bar) {
        pineforge::source::SeriesHighestBars series;
        series.compute(bar.high, length);
    }
};

}  // namespace

int main() {
    // Five invented bars, open = high = low = close: 5, 7, 4, 8, 6.
    const std::vector<Bar> bars = {
        {5, 5, 5, 1}, {7, 7, 7, 1}, {4, 4, 4, 1}, {8, 8, 8, 1}, {6, 6, 6, 1}};
    // ta.highestbars(high, 1): the highest bar of a one-bar window is the
    // bar itself, offset 0.
    return run_single_length<Fixed, ta::HighestBars, Feed, Twin>(
        "checked_fixed_highestbars", kSite, bars[0], bars[0], 1, bars,
        {0.0, 0.0, 0.0, 0.0, 0.0});
}
