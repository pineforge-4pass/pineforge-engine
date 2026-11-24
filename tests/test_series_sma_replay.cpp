#include <pineforge/source/pine_ta_length.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

int failures = 0;

void check(int cap, const char* name, double got, double expected) {
    if (std::memcmp(&got, &expected, sizeof got) == 0) return;
    std::printf("FAIL cap=%d %s got=%.17g expected=%.17g\n", cap, name, got, expected);
    ++failures;
}

void replay(int cap) {
    const pineforge::detail::TaLookbackScope limit(cap);
    using pineforge::source::SeriesSma;
    constexpr double big = 1e16;
    const double missing = pineforge::na<double>();
    SeriesSma actual, final_only;
    for (auto* site : {&actual, &final_only}) {
        // The next insertion either grows the initial 1024 slots or replaces
        // the oldest source in a wrapped five-slot ring.
        for (int i = 0; i < 1020; ++i) site->compute(0, 5);
        for (double x : {big, 1.0, 1e-16, -big}) site->compute(x, 5);
    }
    // S=0, C=1, suffix [0, big, 1, tiny, -big]. Shrinking on na
    // removes 0 then big; -big + 1 ties to -big.
    check(cap, "na shrink", actual.compute(missing, 3), -big / 3);
    auto checkpoint = actual;
    const double committed = final_only.compute(-1, 5);
    check(cap, "final only", committed, 0.0); // (-1 + 1) / 5
    check(cap, "replay with new length", actual.recompute(-1, 5), 0.0);
    // Shrinking on na removed 0 and big, leaving S=-big,C=1. Replacing
    // the next source by big removes 1 (C=0) and cancels S. The exact
    // suffix [tiny,-big,big] would instead give tiny / 3.
    auto after_na = checkpoint;
    check(cap, "following na shrink", after_na.compute(-big, 3), (-big - big) / 3);
    check(cap, "replace after na shrink", after_na.recompute(big, 3), 0.0);
    check(cap, "finite to na", actual.recompute(missing, 5), 1.0 / 5);
    check(cap, "na to finite", actual.recompute(-1, 5), committed);
    check(cap, "branch", checkpoint.recompute(-2, 5), -1.0 / 5); // (-2 + 1) / 5
    auto resized = actual;
    check(cap, "one retained on na", resized.compute(missing, 1), -1.0);
    // The shrink left S=0,C=-1. Restoring 1,tiny,-big adds tiny then
    // one to C: fl(-1+tiny)+1 = 2^-53. The incoming big cancels S.
    check(cap, "regrow after missing source", resized.compute(big, 5), 0x1p-53 / 5);
    // On the -1 branch, subtracting big loses -1 into C, canceling its 1.
    check(cap, "next final only", final_only.compute(big, 5), 0.0);
    check(cap, "next after replay", actual.compute(big, 5), 0.0);
    actual = checkpoint;
    // On the -2 branch, -big - 2 is exact, so C is still 1.
    check(cap, "next after assignment", actual.compute(big, 5), -1.0 / 5);
    check(cap, "independent checkpoint", checkpoint.recompute(-1, 5), 0.0);
}

} // namespace

int main() {
    replay(0);
    replay(4);
    if (failures) return 1;
    std::puts("PASS series SMA replay and copies across history growth and eviction");
    return 0;
}
