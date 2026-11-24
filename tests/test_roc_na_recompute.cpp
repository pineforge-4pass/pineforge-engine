#include "ta_na_recompute_cases.hpp"

int main() {
    // Percentage changes 8 -> 6, 6 -> 8, and 8 -> 8.
    ta_na_cases::transitions("roc", [] { return pineforge::ta::ROC(1); },
        {2, 8}, 6, 8, pineforge::na<double>(), -25, 100.0 / 3, 0);
    return ta_na_cases::failures != 0;
}
