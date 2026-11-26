// A simple moving average built with a length of zero or less has no window:
// every call answers na and no source ever enters the window.
#include "ta_invalid_length_cases.hpp"

#include <cstddef>

namespace {

// The window is private: read the count of sources that entered it.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Window { friend auto member(Window); };
template struct Access<Window, &pineforge::ta::SMA::window_>;

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::ta::SMA average(length);
    const auto entered = [&average] {
        return static_cast<std::size_t>((average.*member(Window{})).count());
    };

    calling("sma", label, "compute(4)");
    expect_na("sma", label, "compute(4)", average.compute(4.0));
    expect_nothing_held("sma", label, "after compute(4)", entered());

    calling("sma", label, "compute(6.5)");
    expect_na("sma", label, "compute(6.5)", average.compute(6.5));
    calling("sma", label, "compute(na)");
    expect_na("sma", label, "compute(na)", average.compute(missing));
    calling("sma", label, "recompute(5.25)");
    expect_na("sma", label, "recompute(5.25)", average.recompute(5.25));
    calling("sma", label, "recompute(na)");
    expect_na("sma", label, "recompute(na)", average.recompute(missing));
    expect_nothing_held("sma", label, "after the last call", entered());
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
