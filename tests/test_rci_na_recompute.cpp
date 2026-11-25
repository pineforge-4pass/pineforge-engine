#include "ta_na_recompute_cases.hpp"

int main() {
    // RCI uses Spearman's average ranks for ties. Falling and rising pairs give -100 and 100.
    // expectation corrected: 50 -> na, because a flat window has zero price-rank variance.
    // Agreement with TradingView's RCI values is unverified.
    ta_na_cases::transitions("rci", [] { return pineforge::ta::RCI(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), -100, 100, pineforge::na<double>());
    return ta_na_cases::failures != 0;
}
