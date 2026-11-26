// The Bollinger composites hold a simple moving average and a standard
// deviation of one length. With a length of zero or less neither has a window,
// so the three bands and their width are na on every call.
#include "ta_invalid_length_cases.hpp"

namespace {

void bands(int length, const char* label) {
    using namespace ta_invalid_length;
    pineforge::ta::BB band(length, 2.0);
    const auto all_na = [&](const char* call, const pineforge::ta::BBResult& got) {
        expect_na("bb middle", label, call, got.middle);
        expect_na("bb upper", label, call, got.upper);
        expect_na("bb lower", label, call, got.lower);
    };
    calling("bb", label, "compute(4)");
    all_na("compute(4)", band.compute(4.0));
    calling("bb", label, "compute(6.5)");
    all_na("compute(6.5)", band.compute(6.5));
    calling("bb", label, "recompute(5.25)");
    all_na("recompute(5.25)", band.recompute(5.25));
}

void width(int length, const char* label) {
    using namespace ta_invalid_length;
    pineforge::ta::BBW spread(length, 2.0);
    calling("bbw", label, "compute(4)");
    expect_na("bbw", label, "compute(4)", spread.compute(4.0));
    calling("bbw", label, "compute(6.5)");
    expect_na("bbw", label, "compute(6.5)", spread.compute(6.5));
    calling("bbw", label, "recompute(5.25)");
    expect_na("bbw", label, "recompute(5.25)", spread.recompute(5.25));
}

}  // namespace

int main() {
    // Length 0 first: the moving average inside is computed first, and its
    // empty window has no buffer at all.
    bands(0, "length 0");
    width(0, "length 0");
    bands(-3, "length -3");
    width(-3, "length -3");
    return ta_invalid_length::result();
}
