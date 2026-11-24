#include "ta_na_recompute_cases.hpp"

int main() {
    // Squared deviations sum to 2 for {8,6} and {6,8}, and 0 for {8,8}.
    for (bool biased : {true, false}) {
        const double variance = biased ? 1 : 2;
        ta_na_cases::transitions(biased ? "population variance" : "sample variance",
            [=] { return pineforge::ta::Variance(2, biased); },
            {2, 8}, 6, 8, pineforge::na<double>(), variance, variance, 0);
    }
    return ta_na_cases::failures != 0;
}
