// A population standard deviation built with a length of zero or less has no
// window: every call answers na and nothing enters either running sum.
#include "ta_invalid_length_cases.hpp"

#include <cstddef>

namespace {

// The two running sums are private: read the count of sources that entered
// each.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Sources { friend auto member(Sources); };
struct Squares { friend auto member(Squares); };
template struct Access<Sources, &pineforge::ta::StdDev::sum_x_>;
template struct Access<Squares, &pineforge::ta::StdDev::sum_xx_>;

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::StdDev deviation(length);  // the population form is the default
    const auto entered = [&deviation] {
        return static_cast<std::size_t>((deviation.*member(Sources{})).count())
             + static_cast<std::size_t>((deviation.*member(Squares{})).count());
    };

    calling("stdev", label, "compute(4)");
    expect_na("stdev", label, "compute(4)", deviation.compute(4.0));
    expect_nothing_held("stdev", label, "after compute(4)", entered());

    calling("stdev", label, "compute(6.5)");
    expect_na("stdev", label, "compute(6.5)", deviation.compute(6.5));
    calling("stdev", label, "compute(na)");
    expect_na("stdev", label, "compute(na)", deviation.compute(missing));
    calling("stdev", label, "recompute(5.25)");
    expect_na("stdev", label, "recompute(5.25)", deviation.recompute(5.25));
    calling("stdev", label, "recompute(na)");
    expect_na("stdev", label, "recompute(na)", deviation.recompute(missing));
    expect_nothing_held("stdev", label, "after the last call", entered());
}

}  // namespace

int main() {
    // Length 0 first: an empty window has no buffer at all, so a source that
    // enters it is read and written through a null buffer.
    row(0, "length 0");
    row(-1, "length -1");
    row(-7, "length -7");
    return ta_invalid_length::result();
}
