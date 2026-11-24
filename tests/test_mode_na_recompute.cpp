#include "ta_na_recompute_cases.hpp"

int main() {
    // Each pair ties on frequency. The smaller of {8,6}, {6,10}, {8,10} wins.
    ta_na_cases::transitions("mode", [] { return pineforge::ta::Mode(2); },
        {2, 8}, 6, 10, pineforge::na<double>(), 6, 6, 8);
    return ta_na_cases::failures != 0;
}
