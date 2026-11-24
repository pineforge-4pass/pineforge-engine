// ta.rci over a window long enough that n * (n * n - 1) no longer fits in a
// 32-bit int.
//
// Strictly falling prices rank exactly opposite to time, so the Spearman
// coefficient 1 - 6 * sum(d^2) / (n * (n^2 - 1)) is exactly -1 and the
// indicator is exactly -100 for every window length. With time rank t and
// price rank n - 1 - t, d = 2t - (n - 1) and sum(d^2) = n * (n^2 - 1) / 3;
// every intermediate value is an integer below 2^53, so the result is exact.
#include <pineforge/na.hpp>
#include <pineforge/ta.hpp>

#include <cstdio>

using namespace pineforge;

namespace {

int failures = 0;

void falling_window(int length) {
    ta::RCI rci(length);
    double last = na<double>();
    for (int bar = 0; bar < length; ++bar) {
        last = rci.compute(static_cast<double>(length - bar));
    }
    if (!is_na(last) && last == -100.0) {
        std::printf("PASS falling window of %d bars: %.17g\n", length, last);
    } else {
        std::printf("FAIL falling window of %d bars: got %.17g, want -100\n",
                    length, last);
        ++failures;
    }
}

}  // namespace

int main() {
    // 1290 * (1290^2 - 1) = 2,146,687,710 still fits in an int;
    // 1291 * (1291^2 - 1) = 2,151,683,880 is above 2,147,483,647.
    falling_window(1290);
    falling_window(1291);
    return failures == 0 ? 0 : 1;
}
