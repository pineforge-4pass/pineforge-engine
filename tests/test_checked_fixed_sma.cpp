// Fixed-length ta.sma (source::PineFixedSma): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, "Invalid value of the 'length'
// argument (0) in the 'sma' function. It must be > 0.", the one a series
// length of the same value raises (source::SeriesSma), and a valid length
// computes ta::SMA's values. ta::SMA itself takes a length of 0: its window
// then assumes a positive length it does not have.
// Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.sma call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedSma;
#else
using Fixed = ta::SMA;
#endif

// TradingView names the argument and the function as the call does.
const Site kSite{"sma", "length", "'sma'", "length", "> 0"};

// ta.sma(close, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) { return call.compute(bar.close); }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) { return call.recompute(bar.close); }
};

// ta.sma(close, length) with a series length.
struct Twin {
    static constexpr bool present = true;
    static void call(double length, const Bar& bar) {
        pineforge::source::SeriesSma series;
        series.compute(bar.close, length);
    }
};

}  // namespace

int main() {
    // Five invented bars, open = high = low = close: 4, 7, 2, 9, 5.
    const std::vector<Bar> bars = {
        {4, 4, 4, 1}, {7, 7, 7, 1}, {2, 2, 2, 1}, {9, 9, 9, 1}, {5, 5, 5, 1}};
    // The zero-length rows use a bar whose source is na. The Pine stop does
    // not depend on the source, and ta::SMA with a length of 0 only holds its
    // (empty) window on such a bar, where a present source would make it read
    // a slot the window does not have. The other stop rows use the first of
    // the five bars: they only run on the adapter, which builds no ta::SMA
    // from an invalid length.
    const Bar missing{na<double>(), na<double>(), na<double>(), 1};
    // ta.sma(close, 2): na, 5.5, 4.5, 5.5, 7.
    return run_single_length<Fixed, ta::SMA, Feed, Twin>(
        "checked_fixed_sma", kSite, missing, bars[0], 2, bars,
        {na<double>(), 5.5, 4.5, 5.5, 7.0});
}
