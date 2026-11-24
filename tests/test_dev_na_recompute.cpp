#include "ta_na_recompute_cases.hpp"

int main() {
    // Both values lie one away from the mean of {8,6}; {8,8} has no spread.
    ta_na_cases::transitions("dev", [] { return pineforge::ta::Dev(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), 1, 1, 0);
    return ta_na_cases::failures != 0;
}
