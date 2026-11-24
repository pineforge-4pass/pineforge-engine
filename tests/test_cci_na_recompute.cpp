#include "ta_na_recompute_cases.hpp"

int main() {
    // {8,6} and {6,8} have mean 7 and mean deviation 1; {8,8} is flat.
    ta_na_cases::transitions("cci", [] { return pineforge::ta::CCI(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), -1 / 0.015, 1 / 0.015, 0);
    return ta_na_cases::failures != 0;
}
