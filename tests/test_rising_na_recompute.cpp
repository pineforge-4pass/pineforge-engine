#include "ta_na_recompute_cases.hpp"

int main() {
    // 6 does not rise above 8; 8 rises above 6 but not above 8.
    ta_na_cases::transitions("rising", [] { return pineforge::ta::Rising(1); },
        {2, 8}, 6, 8, 0, 0, 1, 0);
    return ta_na_cases::failures != 0;
}
