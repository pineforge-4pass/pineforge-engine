#include "ta_na_recompute_cases.hpp"

int main() {
    // Falling and rising ranks give -100 and 100. With {8,8}, both ranks
    // are 1/2, sum(d²) = 1/2 and 100 * (1 - 6 * (1/2) / 6) = 50.
    ta_na_cases::transitions("rci", [] { return pineforge::ta::RCI(2); },
        {2, 8}, 6, 8, pineforge::na<double>(), -100, 100, 50);
    return ta_na_cases::failures != 0;
}
