#include "ta_na_recompute_cases.hpp"

int main() {
    // Falling and rising ranks give -100 and 100.
    // expectation corrected: 50 -> na, because TradingView's ta.rci is na on a flat window (recorded 2026-11-25)
    ta_na_cases::transitions("rci", [] { return pineforge::ta::RCI(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), -100, 100, pineforge::na<double>());
    return ta_na_cases::failures != 0;
}
