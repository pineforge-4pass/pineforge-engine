// A rolling sum built with a length of zero or less has no window: every call
// answers na and no source ever enters the window.
#include "ta_invalid_length_cases.hpp"

#include <pineforge/math.hpp>

#include <cstddef>

namespace {

// The window is private: read the count of sources that entered it.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct Window { friend auto member(Window); };
template struct Access<Window, &pineforge::math::Sum::window_>;

void row(int length, const char* label) {
    using namespace ta_invalid_length;
    const double missing = pineforge::na<double>();
    pineforge::math::Sum total(length);
    const auto entered = [&total] {
        return static_cast<std::size_t>((total.*member(Window{})).count());
    };

    calling("sum", label, "compute(4)");
    expect_na("sum", label, "compute(4)", total.compute(4.0));
    expect_nothing_held("sum", label, "after compute(4)", entered());

    calling("sum", label, "compute(6.5)");
    expect_na("sum", label, "compute(6.5)", total.compute(6.5));
    calling("sum", label, "compute(na)");
    expect_na("sum", label, "compute(na)", total.compute(missing));
    // A replacement source is pushed on its own path, not through compute().
    calling("sum", label, "recompute(5.25)");
    expect_na("sum", label, "recompute(5.25)", total.recompute(5.25));
    calling("sum", label, "recompute(na)");
    expect_na("sum", label, "recompute(na)", total.recompute(missing));
    expect_nothing_held("sum", label, "after the last call", entered());
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
