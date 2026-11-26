// Fixed-length ta.atr (source::PineFixedAtr): a length of 0, a negative
// length or an na length that is a constant or an input stops the call's
// first execution with TradingView's error, which is raised in the ta.rma
// that ta.atr calls: "Invalid value of the 'length' argument (0) in the
// 'atr'->'rma' function. It must be > 0.". A simple length of the same value
// raises the same coded failure and keeps the released text, which names the
// call's own function: "... in the 'atr' function. It must be > 0.". A valid
// length computes ta::ATR's values, in the three-argument form and in the
// four-argument form a call site that does not run on every bar uses (the
// previous chart close handed in). ta::ATR itself takes a length of 0 and
// answers na on every bar.
// Rows: tests/checked_fixed_length_rows.hpp.
#include "checked_fixed_length_rows.hpp"

namespace {

using namespace checked_fixed_length;

// The type a ta.atr call with a constant or input length is built with.
#ifdef PINEFORGE_HAS_PINE_FIXED_LENGTH
using Fixed = pineforge::source::PineFixedAtr;
#else
using Fixed = ta::ATR;
#endif

// The call is ta.atr(length); TradingView's text names the function as the
// ta.rma inside it.
const Site kSite{"atr", "length", "'atr'->'rma'", "length", "> 0"};

// ta.atr(length) on a call site that runs on every bar.
struct Feed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) {
        return call.compute(bar.high, bar.low, bar.close);
    }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) {
        return call.recompute(bar.high, bar.low, bar.close);
    }
};

// ta.atr(length) on a call site that is given the previous chart close: an
// invented one here, a quarter above the bar's low.
struct ChartCloseFeed {
    template <class Call>
    static double compute(Call& call, const Bar& bar) {
        return call.compute(bar.high, bar.low, bar.close, bar.low + 0.25);
    }
    template <class Call>
    static double recompute(Call& call, const Bar& bar) {
        return call.recompute(bar.high, bar.low, bar.close, bar.low + 0.25);
    }
};

}  // namespace

int main() {
    // Five invented bars around a close of 10: the true ranges are 2, 4, 2,
    // 6 and 2.
    const std::vector<Bar> bars = {{11, 9, 10, 1}, {12, 8, 10, 1}, {11, 9, 10, 1},
                                   {13, 7, 10, 1}, {11, 9, 10, 1}};
    // ta.atr(2): na, (2 + 4) / 2, then (true range + previous) / 2.
    const int result = run_single_length<Fixed, ta::ATR, Feed, NoSeriesTwin>(
        "checked_fixed_atr", kSite, bars[0], bars[0], 2, bars,
        {na<double>(), 3.0, 2.5, 4.25, 3.125});
    if (result != 0) return result;

    // The four-argument form stops and computes alike.
    invalid_lengths_stop<Fixed, ChartCloseFeed>(kSite, bars[0]);
    valid_length_is_the_wrapped_type<Fixed, ta::ATR, ChartCloseFeed>(kSite);
    return finish("checked_fixed_atr, previous chart close handed in");
}
