#include "ta_na_recompute_cases.hpp"

int main() {
    // The median of two samples is their midpoint: {8,6}, {6,8}, or {8,8}.
    ta_na_cases::transitions("median", [] { return pineforge::ta::Median(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), 7, 7, 8);
    return ta_na_cases::failures != 0;
}
