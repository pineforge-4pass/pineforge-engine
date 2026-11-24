#include "ta_na_recompute_cases.hpp"

int main() {
    // 6 - 8, then 8 - 6; a skipped bar instead leaves 8 - 8.
    ta_na_cases::transitions("mom", [] { return pineforge::ta::Mom(1); },
        {2, 8}, 6, 8, pineforge::na<double>(), -2, 2, 0);
    return ta_na_cases::failures != 0;
}
