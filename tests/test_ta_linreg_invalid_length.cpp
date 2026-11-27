// A linear regression built with a negative length has no window: every call
// answers na and no value is ever kept. A length of 0 answers na as well: its
// one value leaves as it enters.
#include "ta_invalid_length_cases.hpp"

namespace {

// The window is private: read how many values it holds.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Window { friend auto member(Window); };
template struct Access<Window, &pineforge::ta::Linreg::buffer_>;

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::Linreg line(length);
    const auto held = [&line] { return (line.*member(Window{})).size(); };

    // Two calls before the window is inspected: the second one stores its
    // value in whatever the first one left of the window.
    calling("linreg", label, "compute(4, 0)");
    const double first = line.compute(4.0, 0.0);
    calling("linreg", label, "compute(6.5, 0)");
    const double second = line.compute(6.5, 0.0);
    expect_na("linreg", label, "compute(4, 0)", first);
    expect_na("linreg", label, "compute(6.5, 0)", second);
    expect_nothing_held("linreg", label, "after two calls", held());

    calling("linreg", label, "compute(na, 0)");
    expect_na("linreg", label, "compute(na, 0)", line.compute(missing, 0.0));
    calling("linreg", label, "recompute(5.25, 1)");
    expect_na("linreg", label, "recompute(5.25, 1)", line.recompute(5.25, 1.0));
    expect_nothing_held("linreg", label, "after the last call", held());
}

}  // namespace

int main() {
    // The longest trim first: with a length of -100 the loop that trims the
    // window asks for a hundred more values than the one it holds.
    row(-100, "length -100");
    row(-1, "length -1");
    row(0, "length 0");
    return ta_invalid_length::result();
}
