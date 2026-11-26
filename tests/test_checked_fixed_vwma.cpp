// Fixed-length ta.vwma (source::PineFixedVwma): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, which is raised in the ta.sma
// that ta.vwma calls: "Invalid value of the 'length' argument (0) in the
// 'vwma'->'sma' function. It must be > 0.". A simple length of the same value
// raises the same coded failure and keeps the released text, which names the
// call's own function: "... in the 'vwma' function. It must be > 0.". A valid
// length computes the values of source::PineVwma, the Pine type a ta.vwma
// call is built with: ta.sma(source * volume, length) /
// ta.sma(volume, length), two windows that take their inputs separately.
// PineVwma itself takes a length of 0: its two ta::SMA windows then assume a
// positive length they do not have.
// Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.vwma call with a valid length answers as.
using Wrapped = pineforge::source::PineVwma;

// The type a ta.vwma call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedVwma;
#else
using Fixed = Wrapped;
#endif

// The call is ta.vwma(source, length); TradingView's text names the function
// as the ta.sma inside it.
const Site kSite{"vwma", "length", "'vwma'->'sma'", "length", "> 0"};

// ta.vwma(close, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) {
        return call.compute(bar.close, bar.volume);
    }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) {
        return call.recompute(bar.close, bar.volume);
    }
};

}  // namespace

int main() {
    const double none = na<double>();
    // Five invented bars, open = high = low = close: 3, 5, 4, 8, 6, with
    // volumes 1, 3, 1, 1, 3.
    const std::vector<Bar> bars = {
        {3, 3, 3, 1}, {5, 5, 5, 3}, {4, 4, 4, 1}, {8, 8, 8, 1}, {6, 6, 6, 3}};
    // The zero-length rows use a bar whose volume is na. The Pine stop does
    // not depend on the bar, and PineVwma with a length of 0 only holds its
    // two (empty) ta::SMA windows on such a bar: close * volume and volume
    // are both na there, where a present input would make ta::SMA read a slot
    // the window does not have. The other stop rows use the first of the five
    // bars: they only run on the adapter, which builds no PineVwma from an
    // invalid length.
    const Bar missing{3, 3, 3, none};
    // ta.vwma(close, 2): na, (3 + 15) / 2 over (1 + 3) / 2 = 4.5,
    // (15 + 4) / 2 over (3 + 1) / 2 = 4.75, (4 + 8) / 2 over (1 + 1) / 2 = 6,
    // (8 + 18) / 2 over (1 + 3) / 2 = 6.5.
    const int result = run_single_length<Fixed, Wrapped, Feed, NoSeriesTwin>(
        "checked_fixed_vwma", kSite, missing, bars[0], 2, bars, {none, 4.5, 4.75, 6.0, 6.5});
    if (result != 0) return result;

    // Bars the Pine type decides, ta.vwma(close, 2): the close is na on bar 2
    // while the volume is not.
    //   close * volume  8  12  na  18  10      volume  2  2  6  2  2
    // Bar 1: (8 + 12) / 2 over (2 + 2) / 2 = 5. Bar 2: the product window
    // holds its mean of 10 and the volume window takes the 6: 10 over
    // (2 + 6) / 2 = 2.5, where ta::VWMA, which keeps complete pairs, has no
    // value. Bar 3: (12 + 18) / 2 over (6 + 2) / 2 = 3.75. Bar 4:
    // (18 + 10) / 2 over (2 + 2) / 2 = 7.
    const std::vector<Bar> gap = {
        {4, 4, 4, 2}, {6, 6, 6, 2}, {none, none, none, 6}, {9, 9, 9, 2}, {5, 5, 5, 2}};
    pine_type_values<Fixed, ta::VWMA, Feed>(kSite, 2, gap, {none, 5.0, 2.5, 3.75, 7.0});
    return finish("checked_fixed_vwma, the Pine type");
}
