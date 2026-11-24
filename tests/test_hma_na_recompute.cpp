#include "ta_na_recompute_cases.hpp"

int main() {
    // For four inputs a,b,c,d the last WMA receives
    // 2*(c+2d)/3 - (a+2b+3c+4d)/10. On 1..7 these inputs are
    // 13/3, 16/3, 19/3, 22/3: the two-point weighted means end at 6, 7.
    // Skipping 6 instead gives 109/15 on 3,4,5,7 after 16/3 on 2,3,4,5.
    ta_na_cases::transitions("hma", [] { return pineforge::ta::HMA(4); },
        {1, 2, 3, 4, 5}, 6, 7, pineforge::na<double>(), 6, 7, 298.0 / 45);
    return ta_na_cases::failures != 0;
}
