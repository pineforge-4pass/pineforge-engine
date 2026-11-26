// Fixed-length ta.rma (source::PineFixedRma): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, "Invalid value of the 'length'
// argument (0) in the 'rma' function. It must be > 0.", and a valid length
// computes ta::RMA's values. ta::RMA itself takes a length of 0 and answers
// na on every bar. Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.rma call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedRma;
#else
using Fixed = ta::RMA;
#endif

// TradingView names the argument and the function as the call does.
const Site kSite{"rma", "length", "'rma'", "length", "> 0"};

// ta.rma(close, length).
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) { return call.compute(bar.close); }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) { return call.recompute(bar.close); }
};

}  // namespace

int main() {
    // Five invented bars, open = high = low = close: 4, 7, 2, 9, 5.
    const std::vector<Bar> bars = {
        {4, 4, 4, 1}, {7, 7, 7, 1}, {2, 2, 2, 1}, {9, 9, 9, 1}, {5, 5, 5, 1}};
    // ta.rma(close, 2): na, the mean of the first two, then
    // (close + previous) / 2.
    return run_single_length<Fixed, ta::RMA, Feed, NoSeriesTwin>(
        "checked_fixed_rma", kSite, bars[0], bars[0], 2, bars,
        {na<double>(), 5.5, 3.75, 6.375, 5.6875});
}
