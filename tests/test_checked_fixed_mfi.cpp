// Fixed-length ta.mfi (source::PineFixedMfi): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, "Invalid value of the 'length'
// argument (0) in the 'mfi' function. It must be > 0.", and a valid length
// computes the values of source::PineMfi, the Pine type a ta.mfi call is
// built with: ta::MFI's values with TradingView's first value, one bar
// earlier. PineMfi itself takes a length of 0 and answers 100 on every bar,
// as the ta::MFI inside it does. Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.mfi call with a valid length answers as.
using Wrapped = pineforge::source::PineMfi;

// The type a ta.mfi call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedMfi;
#else
using Fixed = Wrapped;
#endif

// TradingView names the argument and the function as the call does.
const Site kSite{"mfi", "length", "'mfi'", "length", "> 0"};

// ta.mfi(close, length).
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
    // Five invented bars, open = high = low = close: 3, 6, 4, 8, 6, each with
    // a volume of 1.
    const std::vector<Bar> bars = {
        {3, 3, 3, 1}, {6, 6, 6, 1}, {4, 4, 4, 1}, {8, 8, 8, 1}, {6, 6, 6, 1}};
    // ta.mfi(close, 1): the first bar's flow of 3 counts on both sides, so
    // the first value is 100 - 100 / (1 + 3 / 3) = 50 on the first bar; then
    // 100 after a rise and 0 after a fall.
    const int result = run_single_length<Fixed, Wrapped, Feed, NoSeriesTwin>(
        "checked_fixed_mfi", kSite, bars[0], bars[0], 1, bars, {50.0, 100.0, 0.0, 100.0, 0.0});
    if (result != 0) return result;

    // Bars the Pine type decides, closes 3, 6, 4, 16, 4 with a volume of 1,
    // ta.mfi(close, 2). Bar 1 is the first value: the first bar's flow of 3
    // on both sides and the rise of 6 give 100 - 100 / (1 + 9 / 3) = 75,
    // where ta::MFI has no value yet. Then one rise and one fall per window:
    // 6 over 4 gives 100 - 100 / 2.5 = 60, and 16 over 4 gives
    // 100 - 100 / 5 = 80 on the last two bars.
    const std::vector<Bar> first_value = {
        {3, 3, 3, 1}, {6, 6, 6, 1}, {4, 4, 4, 1}, {16, 16, 16, 1}, {4, 4, 4, 1}};
    pine_type_values<Fixed, ta::MFI, Feed>(kSite, 2, first_value,
                                           {na<double>(), 75.0, 60.0, 80.0, 80.0});
    return finish("checked_fixed_mfi, the Pine type");
}
