#include "ta_na_recompute_cases.hpp"

int main() {
    // -6 does not fall below -8; -8 falls below -6 but not below -8.
    ta_na_cases::transitions("falling", [] { return pineforge::ta::Falling(1); },
        {-2, -8}, -6, -8, 0, 0, 1, 0);
    return ta_na_cases::failures != 0;
}
