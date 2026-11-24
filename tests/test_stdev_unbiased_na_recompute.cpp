// Unbiased ta.stdev when a bar is calculated again and its value changes to
// or from na. A recalculation re-runs the current bar on the state the
// earlier bars left, so it must give what an ordinary run over the same final
// values gives, and it must not change the values of earlier bars. An na
// value never enters the window; once `length` values were seen, an na bar
// holds the deviation of the last `length` values.
//
// Length 2 with earlier values 1 and 3 (sample deviation, divisor n - 1 = 1):
//   window {1, 3}: mean 2, deviation sqrt((1 + 1) / 1) = sqrt(2)
//   window {3, 5}: mean 4, deviation sqrt((1 + 1) / 1) = sqrt(2)
//   window {5, 7}: mean 6, deviation sqrt((1 + 1) / 1) = sqrt(2)
//   window {3, 7}: mean 5, deviation sqrt((4 + 4) / 1) = sqrt(8)
//   window {3, 9}: mean 6, deviation sqrt((9 + 9) / 1) = sqrt(18)
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>

using namespace pineforge;

namespace {

int failures = 0;

void expect(const char* row, double got, double want) {
    if (!is_na(got) && std::fabs(got - want) <= 1e-12) {
        std::printf("PASS %s: %.17g\n", row, got);
    } else {
        std::printf("FAIL %s: got %.17g, want %.17g\n", row, got, want);
        ++failures;
    }
}

ta::StdDev after_one_and_three() {
    ta::StdDev deviation(2, false);
    deviation.compute(1.0);
    deviation.compute(3.0);
    return deviation;
}

}  // namespace

int main() {
    {
        // The bar is first na, then 5: an ordinary run over 1, 3, 5.
        ta::StdDev deviation = after_one_and_three();
        expect("na bar holds the window {1, 3}",
               deviation.compute(na<double>()), std::sqrt(2.0));
        expect("na bar recalculated as 5 reads {3, 5}",
               deviation.recompute(5.0), std::sqrt(2.0));
        expect("next bar 7 reads {5, 7}",
               deviation.compute(7.0), std::sqrt(2.0));
    }
    {
        // The bar is first 9, then na: an ordinary run over 1, 3, na.
        ta::StdDev deviation = after_one_and_three();
        expect("bar 9 reads {3, 9}",
               deviation.compute(9.0), std::sqrt(18.0));
        expect("bar 9 recalculated as na holds {1, 3}",
               deviation.recompute(na<double>()), std::sqrt(2.0));
        expect("next bar 7 after the na bar reads {3, 7}",
               deviation.compute(7.0), std::sqrt(8.0));
    }
    {
        // The bar is na in both calculations: an ordinary run over 1, 3, na.
        ta::StdDev deviation = after_one_and_three();
        expect("na bar holds {1, 3}",
               deviation.compute(na<double>()), std::sqrt(2.0));
        expect("na bar recalculated as na still holds {1, 3}",
               deviation.recompute(na<double>()), std::sqrt(2.0));
        expect("next bar 5 after the na bar reads {3, 5}",
               deviation.compute(5.0), std::sqrt(2.0));
    }
    return failures == 0 ? 0 : 1;
}
