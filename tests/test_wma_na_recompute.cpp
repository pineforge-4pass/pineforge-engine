#include "ta_na_recompute_cases.hpp"

int main() {
    // Oldest weight 1, newest weight 2: (8+12)/3, (6+16)/3, (8+16)/3.
    ta_na_cases::transitions("wma", [] { return pineforge::ta::WMA(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), 20.0 / 3, 22.0 / 3, 8);
    return ta_na_cases::failures != 0;
}
