// A running moving average built with a length of zero or less has no seed
// window and no recursion: every call answers na. The smallest int is one of
// those lengths (it is also the integer na), and `length - 1` cannot be
// formed from it, so the answer must be given before that subtraction.
#include "ta_invalid_length_cases.hpp"

#include <limits>

namespace {

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::RMA average(length);

    calling("rma", label, "compute(4)");
    expect_na("rma", label, "compute(4)", average.compute(4.0));
    calling("rma", label, "compute(6.5)");
    expect_na("rma", label, "compute(6.5)", average.compute(6.5));
    calling("rma", label, "compute(na)");
    expect_na("rma", label, "compute(na)", average.compute(missing));
    calling("rma", label, "recompute(5.25)");
    expect_na("rma", label, "recompute(5.25)", average.recompute(5.25));
}

}  // namespace

int main() {
    row(std::numeric_limits<int>::min(), "smallest int length");
    row(-1, "length -1");
    row(0, "length 0");
    return ta_invalid_length::result();
}
