// A zero-length sample window has neither a deviation nor a value to undo.
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cstdio>

namespace stdev_empty_test {

// Inspect the rewind record before replay: an unchecked empty deque pop
// need not trap, and its numerical result alone cannot establish safety.
struct Pushed { friend auto member(Pushed); };
template<class Tag, auto Member>
struct Read { friend auto member(Tag) { return Member; } };
template struct Read<Pushed, &pineforge::ta::StdDev::pushed_>;

}  // namespace stdev_empty_test

int main() {
    using namespace pineforge;
    using namespace stdev_empty_test;
    ta::StdDev deviation(0, false);
    if (!is_na(deviation.compute(1.0))) {
        std::puts("FAIL zero-length sample deviation: expected na");
        return 1;
    }
    if (deviation.*member(Pushed{})) {
        std::puts("FAIL zero-length sample deviation: marked an empty window for undo");
        return 1;
    }
    if (!is_na(deviation.recompute(2.0)) ||
        !is_na(deviation.recompute(na<double>()))) {
        std::puts("FAIL zero-length sample deviation: expected na on replay");
        return 1;
    }
    std::puts("PASS zero-length sample deviation: no sample to undo, replay returns na");
}
