// A pivot high built with a negative bar count on either side describes no
// window: every call answers na and no value is ever kept.
#include "ta_invalid_length_cases.hpp"

namespace {

// The window is private: read how many values it holds.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Window { friend auto member(Window); };
template struct Access<Window, &pineforge::ta::PivotHigh::buffer_>;

void row(int left_bars, int right_bars, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::PivotHigh pivot(left_bars, right_bars);
    const auto held = [&pivot] { return (pivot.*member(Window{})).size(); };

    calling("pivothigh", label, "compute(4)");
    expect_na("pivothigh", label, "compute(4)", pivot.compute(4.0));
    expect_nothing_held("pivothigh", label, "after compute(4)", held());

    calling("pivothigh", label, "compute(6.5)");
    expect_na("pivothigh", label, "compute(6.5)", pivot.compute(6.5));
    calling("pivothigh", label, "compute(3.25)");
    expect_na("pivothigh", label, "compute(3.25)", pivot.compute(3.25));
    calling("pivothigh", label, "recompute(9)");
    expect_na("pivothigh", label, "recompute(9)", pivot.recompute(9.0));
    calling("pivothigh", label, "compute(na)");
    expect_na("pivothigh", label, "compute(na)", pivot.compute(missing));
    expect_nothing_held("pivothigh", label, "after the last call", held());
}

}  // namespace

int main() {
    // The longest trim first: with a left count of -100 the loop that trims
    // the window asks for ninety-nine more values than the one it holds, and
    // the candidate is then read a hundred slots before the start of what is
    // left.
    row(-100, 0, "left -100, right 0");
    row(-1, 1, "left -1, right 1");
    row(0, -1, "left 0, right -1");
    row(2, -3, "left 2, right -3");
    row(-2, -2, "left -2, right -2");
    return ta_invalid_length::result();
}
