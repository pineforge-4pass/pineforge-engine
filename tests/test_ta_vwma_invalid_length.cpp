// A volume-weighted moving average built with a negative length has no
// window: every call answers na and no pair ever enters either buffer. A
// length of 0 answers na as well: its one pair leaves as it enters.
#include "ta_invalid_length_cases.hpp"

namespace {

// The two buffers are private: read how many values each holds.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Products { friend auto member(Products); };
struct Volumes { friend auto member(Volumes); };
template struct Access<Products, &pineforge::ta::VWMA::sv_buffer_>;
template struct Access<Volumes, &pineforge::ta::VWMA::v_buffer_>;

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::VWMA average(length);
    const auto nothing_held = [&](const char* call) {
        expect_nothing_held("vwma products", label, call, (average.*member(Products{})).size());
        expect_nothing_held("vwma volumes", label, call, (average.*member(Volumes{})).size());
    };

    calling("vwma", label, "compute(4, 10)");
    expect_na("vwma", label, "compute(4, 10)", average.compute(4.0, 10.0));
    nothing_held("after compute(4, 10)");

    calling("vwma", label, "compute(6.5, 3)");
    expect_na("vwma", label, "compute(6.5, 3)", average.compute(6.5, 3.0));
    calling("vwma", label, "compute(na, 3)");
    expect_na("vwma", label, "compute(na, 3)", average.compute(missing, 3.0));
    calling("vwma", label, "recompute(5.25, 2)");
    expect_na("vwma", label, "recompute(5.25, 2)", average.recompute(5.25, 2.0));
    calling("vwma", label, "recompute(5.25, na)");
    expect_na("vwma", label, "recompute(5.25, na)", average.recompute(5.25, missing));
    nothing_held("after the last call");
}

}  // namespace

int main() {
    // The longest eviction first: with a length of -100 the loop that takes
    // pairs out of the window asks for a hundred more than the one it holds.
    row(-100, "length -100");
    row(-1, "length -1");
    row(0, "length 0");
    return ta_invalid_length::result();
}
