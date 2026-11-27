// Fixed-length ta.hma (source::PineFixedHma): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, "Invalid value of the 'length'
// argument (0) in the 'hma' function. It must be > 0.", and a valid length
// computes ta::HMA's values. ta::HMA itself takes a length of 0 and answers
// na on every bar. Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.hma call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedHma;
#else
using Fixed = ta::HMA;
#endif

// TradingView names the argument and the function as the call does.
const Site kSite{"hma", "length", "'hma'", "length", "> 0"};

// ta.hma(close, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) { return call.compute(bar.close); }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) { return call.recompute(bar.close); }
};

}  // namespace

int main() {
    // Five invented bars, open = high = low = close: 2, 5, 8, 11, 14.
    const std::vector<Bar> bars = {
        {2, 2, 2, 1}, {5, 5, 5, 1}, {8, 8, 8, 1}, {11, 11, 11, 1}, {14, 14, 14, 1}};
    // ta.hma(close, 1): its three weighted averages have one bar each, so it
    // answers the source.
    return run_single_length<Fixed, ta::HMA, Feed, NoSeriesTwin>(
        "checked_fixed_hma", kSite, bars[0], bars[0], 1, bars, {2.0, 5.0, 8.0, 11.0, 14.0});
}
