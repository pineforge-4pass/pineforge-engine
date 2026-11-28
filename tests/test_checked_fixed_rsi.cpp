// Fixed-length ta.rsi (source::PineFixedRsi): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, which names the length 'y':
// "Invalid value of the 'y' argument (0) in the 'rsi' function. It must be
// > 0.". A simple length of the same value raises the same coded failure and
// keeps the released text, which names the argument as the call does:
// "Invalid value of the 'length' argument (0) in the 'rsi' function. It must
// be > 0.". A valid length computes ta::RSI's values. ta::RSI itself takes a
// length of 0 and answers na on every bar.
// Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.rsi call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedRsi;
#else
using Fixed = ta::RSI;
#endif

// The call's length is the argument `length`; TradingView's text names it y.
const Site kSite{"rsi", "length", "'rsi'", "y", "> 0"};

// ta.rsi(close, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) { return call.compute(bar.close); }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) { return call.recompute(bar.close); }
};

}  // namespace

int main() {
    // Five invented bars, open = high = low = close: 2, 5, 3, 7, 4.
    const std::vector<Bar> bars = {
        {2, 2, 2, 1}, {5, 5, 5, 1}, {3, 3, 3, 1}, {7, 7, 7, 1}, {4, 4, 4, 1}};
    // ta.rsi(close, 1): na on the first bar, then 100 after a rise and 0
    // after a fall.
    return run_single_length<Fixed, ta::RSI, Feed, NoSeriesTwin>(
        "checked_fixed_rsi", kSite, bars[0], bars[0], 1, bars,
        {na<double>(), 100.0, 0.0, 100.0, 0.0});
}
