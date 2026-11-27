// A percent rank built with a negative length has no lookback and no current
// slot: every call answers na and no value is ever kept.
#include "ta_invalid_length_cases.hpp"

namespace {

// The window is private: read how many values it holds.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Window { friend auto member(Window); };
template struct Access<Window, &pineforge::ta::PercentRank::buffer_>;

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::PercentRank rank(length);
    const auto held = [&rank] { return (rank.*member(Window{})).size(); };

    calling("percentrank", label, "compute(4)");
    expect_na("percentrank", label, "compute(4)", rank.compute(4.0));
    expect_nothing_held("percentrank", label, "after compute(4)", held());

    calling("percentrank", label, "compute(6.5)");
    expect_na("percentrank", label, "compute(6.5)", rank.compute(6.5));
    calling("percentrank", label, "compute(na)");
    expect_na("percentrank", label, "compute(na)", rank.compute(missing));
    calling("percentrank", label, "recompute(5.25)");
    expect_na("percentrank", label, "recompute(5.25)", rank.recompute(5.25));
    expect_nothing_held("percentrank", label, "after the last call", held());
}

}  // namespace

int main() {
    // The longest trim first: with a length of -100 the loop that trims the
    // window asks for ninety-nine more values than the one it holds, and the
    // current value is then read from the end of what is left.
    row(-100, "length -100");
    row(-1, "length -1");
    row(-2, "length -2");
    return ta_invalid_length::result();
}
